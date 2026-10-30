#include "faviconcache.h"

#include "remotesource.h"

#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

FaviconCache::FaviconCache(const QString &cacheDir, QObject *parent)
    : QObject(parent), m_dir(cacheDir), m_nam(new QNetworkAccessManager(this)) {
    QDir().mkpath(m_dir);
    // Adopt previously cached files without any network use.
    const QStringList pngs =
        QDir(m_dir).entryList({QStringLiteral("*.png")}, QDir::Files);
    for (const QString &f : pngs) {
        const QString key = f.left(f.size() - 4);
        const QString path = m_dir + u'/' + f;
        m_files.insert(key, path);
        m_icons.insert(key, QIcon(path));
    }
}

QIcon FaviconCache::iconFor(const QString &key, const QString &baseUrl) {
    const QString custom = customPathFor(key);
    if (!custom.isEmpty() && QFile::exists(custom))
        return QIcon(custom);
    if (m_icons.contains(key))
        return m_icons.value(key);
    prefetch(key, baseUrl);
    return {};
}

void FaviconCache::setCustomDir(const QString &dir) {
    if (m_customDir == dir)
        return;
    m_customDir = dir;
    if (m_customDir.isEmpty())
        return;
    // Portable storage: adopt auto-fetched icons into the backup folder,
    // migrating legacy cache-dir files (and their in-memory entries).
    QDir().mkpath(m_customDir + QStringLiteral("/auto"));
    QDir cacheDir(m_dir);
    for (const QString &f :
         cacheDir.entryList({QStringLiteral("*.png")}, QDir::Files)) {
        const QString key = f.left(f.size() - 4);
        const QString dest = autoPathFor(key);
        if (dest.isEmpty() || QFile::exists(dest))
            continue;
        const QString src = m_dir + u'/' + f;
        if (QFile::rename(src, dest)) {
            if (m_files.value(key) == src)
                m_files.insert(key, dest);
            if (m_icons.contains(key)) {
                // QIcons hold their file path: re-point at the moved
                // file (GUI only - headless/test use re-resolves later).
                if (qobject_cast<QGuiApplication *>(
                        QCoreApplication::instance()))
                    m_icons.insert(key, QIcon(dest));
                else
                    m_icons.remove(key);
            }
        }
    }
    QDir autoDir(m_customDir + QStringLiteral("/auto"));
    for (const QString &f :
         autoDir.entryList({QStringLiteral("*.png")}, QDir::Files)) {
        const QString key = f.left(f.size() - 4);
        if (key.isEmpty() || m_icons.contains(key))
            continue;
        const QString path = autoDir.absoluteFilePath(f);
        m_files.insert(key, path);
        m_icons.insert(key, QIcon(path));
    }
}

QString FaviconCache::customPathFor(const QString &key) const {
    if (m_customDir.isEmpty() || key.isEmpty() || key.contains(u'/'))
        return {};
    return m_customDir + u'/' + key + QStringLiteral(".png");
}

QString FaviconCache::autoPathFor(const QString &key) const {
    if (m_customDir.isEmpty() || key.isEmpty() || key.contains(u'/'))
        return {};
    return m_customDir + QStringLiteral("/auto/") + key +
           QStringLiteral(".png");
}

void FaviconCache::prefetch(const QString &key, const QString &baseUrl) {
    if (key.isEmpty() || m_icons.contains(key) || m_dead.contains(key) ||
        m_queued.contains(key) || key == m_currentKey)
        return;
    const QString custom = customPathFor(key);
    if (!custom.isEmpty() && QFile::exists(custom))
        return; // user override wins; no network needed
    if (faviconCandidates(baseUrl).isEmpty()) {
        m_dead.insert(key);
        return;
    }
    m_queue.append({key, baseUrl});
    m_queued.insert(key);
    fetchNext();
}

void FaviconCache::clearPending() {
    if (m_reply) {
        disconnect(m_reply, nullptr, this, nullptr);
        m_reply->abort();
        m_reply->deleteLater();
        m_reply = nullptr;
    }
    m_queue.clear();
    m_queued.clear();
    m_currentKey.clear();
    m_currentCandidates.clear();
}

void FaviconCache::refreshAll() {
    clearPending();
    m_icons.clear();
    m_files.clear();
    m_dead.clear();
    // Auto icons only: custom overrides are user data, never cleared.
    for (const QString &dir :
         {m_dir, m_customDir.isEmpty()
                     ? QString()
                     : m_customDir + QStringLiteral("/auto")}) {
        if (dir.isEmpty())
            continue;
        for (const QString &f :
             QDir(dir).entryList({QStringLiteral("*.png")}, QDir::Files))
            QFile::remove(dir + u'/' + f);
    }
}

void FaviconCache::fetchNext() {
    if (m_reply || m_queue.isEmpty())
        return;
    const QPair<QString, QString> queued = m_queue.takeFirst();
    const QString key = queued.first, base = queued.second;
    m_queued.remove(key);
    m_currentKey = key;
    m_currentCandidates = faviconCandidates(base);
    m_triedUrls.clear();
    // Unconditional: clears a stale proxy when the entry drops it.
    m_nam->setProxy(RemotePosts::proxyFromSpec(
        RemotePosts::proxyForUrl(base)));
    const QUrl url(m_currentCandidates.takeFirst());
    m_triedUrls.insert(url.toString());
    QNetworkRequest req(url);
    req.setTransferTimeout(15000);
    RemoteSource::applyCookies(req); // clearance header, if any
    req.setHeader(QNetworkRequest::UserAgentHeader,
                  QStringLiteral("box/0.1 (local booru browser)"));
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    m_reply = m_nam->get(req);
    connect(m_reply, &QNetworkReply::finished, this,
            &FaviconCache::finishFetch);
}

void FaviconCache::finishFetch() {
    QNetworkReply *reply = m_reply;
    m_reply = nullptr;
    const QString key = m_currentKey;
    bool done = true;
    // Shared continuation: same proxy/cookies/UA as the first fetch.
    auto get = [&](const QUrl &url) {
        m_triedUrls.insert(url.toString());
        QNetworkRequest req(url);
        req.setTransferTimeout(15000);
        RemoteSource::applyCookies(req); // clearance header, if any
        req.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("box/0.1 (local booru browser)"));
        m_reply = m_nam->get(req);
        connect(m_reply, &QNetworkReply::finished, this,
                &FaviconCache::finishFetch);
    };
    if (reply && reply->error() == QNetworkReply::NoError) {
        const QByteArray body = reply->readAll();
        QImage img;
        if (img.loadFromData(body) && !img.isNull()) {
            // Normalize small icons to PNG on disk (portable auto dir
            // when set, legacy cache dir otherwise).
            const QString dir =
                m_customDir.isEmpty()
                    ? m_dir
                    : m_customDir + QStringLiteral("/auto");
            QDir().mkpath(dir);
            const QString path = dir + u'/' + key + QStringLiteral(".png");
            const QImage small =
                img.scaled(32, 32, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            if (small.save(path, "PNG")) {
                m_files.insert(key, path);
                const QIcon icon(path);
                m_icons.insert(key, icon);
                emit ready(key, icon);
            } else {
                m_dead.insert(key);
            }
        } else {
            // Not an image: an HTML 404/landing page may advertise the
            // real icon via <link rel=icon> - queue those (untried only)
            // before falling through to the remaining candidates.
            if (body.toLower().contains("<link")) {
                const QString pageUrl = reply->url().toString();
                for (const QString &found :
                     faviconIconsFromHtml(body, pageUrl)) {
                    if (!m_triedUrls.contains(found) &&
                        !m_currentCandidates.contains(found))
                        m_currentCandidates.append(found);
                }
            }
            if (!m_currentCandidates.isEmpty()) {
                done = false;
                get(QUrl(m_currentCandidates.takeFirst()));
            } else {
                m_dead.insert(key);
            }
        }
    } else if (reply && !m_currentCandidates.isEmpty()) {
        // HTTP error: fall back to the next candidate (e.g. host root).
        done = false;
        get(QUrl(m_currentCandidates.takeFirst()));
    } else {
        m_dead.insert(key);
    }
    if (reply)
        reply->deleteLater();
    if (done) {
        m_currentKey.clear();
        m_currentCandidates.clear();
        m_triedUrls.clear();
        fetchNext();
    }
}
