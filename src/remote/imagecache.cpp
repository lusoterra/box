#include "imagecache.h"

#include "cachejanitor.h"
#include "cachepolicy.h"
#include "remotesource.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStandardPaths>
#include <QUrl>
#include <QtConcurrent>

ImageCache::ImageCache(QObject *parent)
    : QObject(parent), m_nam(new QNetworkAccessManager(this)) {}

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
    if (m_active.contains(url))
        return; // already downloading
    const QString dest = targetPath(url, persist);
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
        } else if (!reply->header(QNetworkRequest::ContentTypeHeader)
                        .toString()
                        .trimmed()
                        .startsWith(QStringLiteral("image/"),
                                    Qt::CaseInsensitive) &&
                   !reply->header(QNetworkRequest::ContentTypeHeader)
                        .toString()
                        .trimmed()
                        .startsWith(QStringLiteral("video/"),
                                    Qt::CaseInsensitive)) {
            // Hotlink/wall pages answer HTML (or 302-to-HTML) when the
            // request lacks clearance: never cache the page as if it
            // were the picture. Some servers serve animated GIFs as
            // video/gif - accept those too. Name a recognized wall
            // and point at cookie capture instead of quoting markup.
            const QString ct =
                reply->header(QNetworkRequest::ContentTypeHeader).toString();
            emit failed(url, RemotePosts::wallAwareMediaError(
                                 QUrl(url).host(), reply->readAll(), ct));
        } else {
            QFile f(dest);
            if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                f.write(reply->readAll());
                f.close();
                if (persist) {
                    // Same off-thread bookkeeping as localPath(): the
                    // enforcement walk must not stall this reply handler.
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
        reply->deleteLater();
    });
}

void ImageCache::drop(const QString &url) {
    cancelUrl(url);
    if (!m_baseDir.isEmpty()) {
        const QString have = targetPath(url, true);
        if (QFile::exists(have))
            QFile::remove(have);
    }
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
