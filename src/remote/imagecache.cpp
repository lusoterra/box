#include "imagecache.h"

#include "cachejanitor.h"
#include "cachepolicy.h"
#include "remotesource.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStandardPaths>
#include <QUrl>
#include <QUuid>
#include <QtConcurrent>

ImageCache::ImageCache(QObject *parent)
    : QObject(parent), m_nam(new QNetworkAccessManager(this)) {
    // One session-only staging dir: RAM bytes live here as files only for
    // as long as Qt's image/video readers need a path. Removed on unload.
    m_sessionTmpDir =
        QStandardPaths::writableLocation(QStandardPaths::TempLocation) +
        QStringLiteral("/box-ram-") +
        QString::number(QCoreApplication::applicationPid()) + u'-' +
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    QDir().mkpath(m_sessionTmpDir);
}

ImageCache::~ImageCache() {
    m_ram.clear();
    m_ramOrder.clear();
    m_ramBytes = 0;
    QDir(m_sessionTmpDir).removeRecursively();
}

QString ImageCache::ramPath(const QString &url) const {
    return m_sessionTmpDir + u'/' +
           QString::fromLatin1(
               QCryptographicHash::hash(url.toUtf8(), QCryptographicHash::Sha1)
                   .toHex()) +
           u'.' + extOfUrl(url);
}

QString ImageCache::materializeRam(const QString &url) const {
    const auto it = m_ram.constFind(url);
    if (it == m_ram.constEnd())
        return {};
    const QString path = ramPath(url);
    if (QFile::exists(path))
        return path; // already staged this session
    QFile f(path);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.write(it.value());
        return path;
    }
    return {};
}

void ImageCache::storeRam(const QString &url, const QByteArray &bytes) {
    if (bytes.isEmpty() || bytes.size() > kRamEntryLimit)
        return; // huge media: still staged to disk for the reader only
    if (m_ram.contains(url))
        m_ramBytes -= m_ram.value(url).size();
    m_ram.insert(url, bytes);
    m_ramOrder.removeAll(url);
    m_ramOrder.append(url); // most-recent last
    m_ramBytes += bytes.size();
    while (m_ramBytes > kRamLimit && m_ramOrder.size() > 1) {
        const QString old = m_ramOrder.takeFirst();
        if (old == url)
            continue; // never evict the entry being stored
        m_ramBytes -= m_ram.value(old).size();
        m_ram.remove(old);
        QFile::remove(ramPath(old));
    }
}

QNetworkAccessManager *ImageCache::namForProxy(const QString &proxySpec) {
    if (proxySpec.trimmed().isEmpty())
        return m_nam;
    auto it = m_proxyNams.find(proxySpec);
    if (it != m_proxyNams.end())
        return it.value();
    const QNetworkProxy proxy = RemotePosts::proxyFromSpec(proxySpec);
    if (proxy.type() == QNetworkProxy::NoProxy)
        return m_nam;
    QNetworkAccessManager *nam = new QNetworkAccessManager(this);
    nam->setProxy(proxy);
    m_proxyNams.insert(proxySpec, nam);
    return nam;
}

void ImageCache::setEnabled(bool on) {
    m_enabled = on;
}

void ImageCache::setBaseDir(const QString &dir) {
    m_baseDir = dir;
    QDir().mkpath(dir);
}

QString ImageCache::extOfUrl(const QString &url) {
    const QString path = QUrl(url).path();
    const int dot = path.lastIndexOf(u'.');
    if (dot < 0)
        return QStringLiteral("bin");
    const QString ext = path.mid(dot + 1).toLower();
    return (ext.size() >= 2 && ext.size() <= 5) ? ext : QStringLiteral("bin");
}

QString ImageCache::targetPath(const QString &url, bool persist) const {
    const QString name =
        QString::fromLatin1(
            QCryptographicHash::hash(url.toUtf8(), QCryptographicHash::Sha1).toHex()) +
        u'.' + extOfUrl(url);
    if (persist && m_enabled && !m_baseDir.isEmpty())
        return m_baseDir + u'/' + name;
    QString tmp = QStandardPaths::writableLocation(QStandardPaths::TempLocation) +
                  QStringLiteral("/box");
    QDir().mkpath(tmp);
    return tmp + u'/' + name;
}

QString ImageCache::localPath(const QString &url, const Post *post) const {
    // Only persistent files count as "cached"; temp files are re-fetched.
    if (!m_enabled || m_baseDir.isEmpty())
        return {};
    // Per-post policy: entries the current mode forbids behave as if the
    // cache did not contain them (so "don't cache this booru" really
    // starts cold instead of silently replaying old disk entries).
    if (post && !MediaCache::persistForPost(*post))
        return {};
    if (!post && !MediaCache::persistFor(QString(), QString()))
        return {};
    const QString p = targetPath(url, true);
    if (!QFile::exists(p))
        return {};
    // Bookkeeping off the UI thread: the janitor is thread-safe, and a
    // switch-heavy session must never wait on SQLite (or the periodic
    // full-cache enforcement walk) between keypress and paint.
    (void)QtConcurrent::run([p] { CacheJanitor::hit(p); });
    return p;
}

void ImageCache::request(const QString &url, const QString &referer,
                         const Post *post) {
    if (url.isEmpty())
        return;
    if (!RemotePosts::isHttpUrl(url)) {
        emit failed(url, tr("Unsupported URL scheme"));
        return;
    }
    const bool persist =
        m_enabled && (post ? MediaCache::persistForPost(*post)
                           : MediaCache::persistFor(QString(), QString()));
    const bool nsfw = post && (MediaCache::isNsfwRating(post->rating) ||
                               MediaCache::postHasBlacklistedTag(*post));
    const QString siteKey = post && post->isRemote ? post->siteKey : QString();
    const QString have = localPath(url, post);
    if (!have.isEmpty()) {
        emit ready(url, have);
        return;
    }
    if (!persist && m_ram.contains(url)) {
        const QString path = materializeRam(url);
        if (!path.isEmpty()) {
            emit ready(url, path);
            return;
        }
    }
    if (m_active.contains(url))
        return; // already downloading
    // Non-persistent mode stages in the session RAM dir (removed on
    // unload), never in the shared temp/cache dir.
    const QString dest = persist ? targetPath(url, persist) : ramPath(url);
    QDir().mkpath(QFileInfo(dest).absolutePath());
    QNetworkRequest req{QUrl(url)};
    req.setTransferTimeout(60000);
    if (!referer.isEmpty())
        req.setRawHeader("Referer", referer.toUtf8());
    RemoteSource::applyCookies(req); // clearance header, if any
    req.setHeader(QNetworkRequest::UserAgentHeader,
                  QStringLiteral("box/0.1 (image cache)"));
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply *reply = namForProxy(RemotePosts::proxyForUrl(url))->get(req);
    m_active.insert(url, reply);
    connect(reply, &QNetworkReply::downloadProgress, this,
            [this, url](qint64 received, qint64 total) {
                if (m_active.contains(url))
                    emit downloadProgress(url, received, total);
            });
    connect(reply, &QNetworkReply::finished, this, [this, url, dest, reply,
                                                    persist, nsfw, siteKey] {
        m_active.remove(url);
        if (reply->error() != QNetworkReply::NoError) {
            emit failed(url, reply->errorString());
        } else {
            const QByteArray bytes = reply->readAll();
            const QString ct =
                reply->header(QNetworkRequest::ContentTypeHeader).toString();
            const QString ctl = ct.trimmed().toLower();
            // Empty/misleading headers are common (Shimmie2 serves JPEGs
            // with no Content-Type): trust the magic bytes before calling
            // it a wall.
            const bool headerMedia =
                ctl.startsWith(QStringLiteral("image/")) ||
                ctl.startsWith(QStringLiteral("video/")) ||
                ctl.startsWith(QStringLiteral("application/octet-stream")) ||
                ctl.startsWith(QStringLiteral("binary/octet-stream"));
            if (!headerMedia &&
                !RemotePosts::looksLikeMediaBytes(bytes.left(512))) {
                // Hotlink/wall pages answer HTML (or 302-to-HTML) when the
                // request lacks clearance: never cache the page as if it
                // were the picture. Name a recognized wall and point at
                // cookie capture instead of quoting markup.
                emit failed(url, RemotePosts::wallAwareMediaError(
                                     QUrl(url).host(), bytes, ct));
            } else {
                QFile f(dest);
                if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                    f.write(bytes);
                    f.close();
                    if (!persist)
                        storeRam(url, bytes); // session-only memory copy
                    if (persist) {
                        // Same off-thread bookkeeping as localPath(): the
                        // enforcement walk must not stall this reply.
                        (void)QtConcurrent::run([dest, nsfw, siteKey] {
                            CacheJanitor::record(dest, nsfw, siteKey);
                            CacheJanitor::enforce();
                        });
                    }
                    emit ready(url, dest);
                } else {
                    emit failed(url, QStringLiteral("Cannot write cache file"));
                }
            }
        }
        reply->deleteLater();
    });
}

void ImageCache::drop(const QString &url) {
    const auto it = m_ram.find(url);
    if (it != m_ram.end()) {
        m_ramBytes -= it.value().size();
        m_ram.erase(it);
    }
    m_ramOrder.removeAll(url);
    QFile::remove(ramPath(url));
    cancelUrl(url);
    if (!m_baseDir.isEmpty()) {
        const QString have = targetPath(url, true);
        if (QFile::exists(have))
            QFile::remove(have);
    }
}

void ImageCache::prefetchRange(const QString &url, const QString &referer,
                               qint64 maxBytes) {
    if (url.isEmpty() || !RemotePosts::isHttpUrl(url) ||
        maxBytes <= 0 || m_prefetched.contains(url) ||
        m_active.contains(url))
        return;
    m_prefetched.insert(url);
    QNetworkRequest req{QUrl(url)};
    req.setTransferTimeout(20000);
    req.setRawHeader("Range",
                     QStringLiteral("bytes=0-%1").arg(maxBytes - 1).toUtf8());
    if (!referer.isEmpty())
        req.setRawHeader("Referer", referer.toUtf8());
    RemoteSource::applyCookies(req);
    req.setHeader(QNetworkRequest::UserAgentHeader,
                  QStringLiteral("box/0.1 (video warm-up)"));
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply *reply =
        namForProxy(RemotePosts::proxyForUrl(url))->get(req);
    connect(reply, &QIODevice::readyRead, reply, [reply, maxBytes] {
        if (reply->bytesAvailable() >= maxBytes)
            reply->abort(); // enough bytes fetched to warm the connection
    });
    connect(reply, &QNetworkReply::finished, reply, [reply] {
        reply->readAll();
        reply->deleteLater();
    });
}

void ImageCache::cancelUrl(const QString &url) {
    auto it = m_active.find(url);
    if (it == m_active.end())
        return;
    disconnect(it.value(), nullptr, this, nullptr);
    it.value()->abort();
    it.value()->deleteLater();
    m_active.erase(it);
    // Drop the partial file if one was started. Both locations are safe
    // to probe: the temp path exists only for policy-denied entries.
    QFile::remove(targetPath(url, true));
    QFile::remove(targetPath(url, false));
}
