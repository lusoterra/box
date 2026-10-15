#include "imagecache.h"

#include "cachejanitor.h"
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

QString ImageCache::targetPath(const QString &url) const {
    const QString name =
        QString::fromLatin1(
            QCryptographicHash::hash(url.toUtf8(), QCryptographicHash::Sha1).toHex()) +
        u'.' + extOfUrl(url);
    if (m_enabled && !m_baseDir.isEmpty())
        return m_baseDir + u'/' + name;
    QString tmp = QStandardPaths::writableLocation(QStandardPaths::TempLocation) +
                  QStringLiteral("/booru-explorer");
    QDir().mkpath(tmp);
    return tmp + u'/' + name;
}

QString ImageCache::localPath(const QString &url) const {
    // Only persistent files count as "cached"; temp files are re-fetched.
    if (!m_enabled || m_baseDir.isEmpty())
        return {};
    const QString p = targetPath(url);
    if (!QFile::exists(p))
        return {};
    // Bookkeeping off the UI thread: the janitor is thread-safe, and a
    // switch-heavy session must never wait on SQLite (or the periodic
    // full-cache enforcement walk) between keypress and paint.
    QtConcurrent::run([p] { CacheJanitor::hit(p); });
    return p;
}

void ImageCache::request(const QString &url, const QString &referer) {
    if (url.isEmpty())
        return;
    const QString have = localPath(url);
    if (!have.isEmpty()) {
        emit ready(url, have);
        return;
    }
    if (m_active.contains(url))
        return; // already downloading
    const QString dest = targetPath(url);
    QDir().mkpath(QFileInfo(dest).absolutePath());
    QNetworkRequest req{QUrl(url)};
    req.setTransferTimeout(60000);
    if (!referer.isEmpty())
        req.setRawHeader("Referer", referer.toUtf8());
    RemoteSource::applyCookies(req); // clearance header, if any
    req.setHeader(QNetworkRequest::UserAgentHeader,
                  QStringLiteral("booru-explorer/0.1 (image cache)"));
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply *reply = m_nam->get(req);
    m_active.insert(url, reply);
    connect(reply, &QNetworkReply::downloadProgress, this,
            [this, url](qint64 received, qint64 total) {
                if (m_active.contains(url))
                    emit downloadProgress(url, received, total);
            });
    connect(reply, &QNetworkReply::finished, this, [this, url, dest, reply] {
        m_active.remove(url);
        if (reply->error() != QNetworkReply::NoError) {
            emit failed(url, reply->errorString());
        } else if (!reply->header(QNetworkRequest::ContentTypeHeader)
                        .toString()
                        .startsWith(QStringLiteral("image/"),
                                    Qt::CaseInsensitive)) {
            // Hotlink walls answer 302-to-HTML when the Referer is missing:
            // never cache the page as if it were the picture.
            emit failed(url, tr("Unexpected content type: %1")
                                 .arg(reply->header(QNetworkRequest::ContentTypeHeader)
                                          .toString()));
        } else {
            QFile f(dest);
            if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                f.write(reply->readAll());
                f.close();
                if (m_enabled) {
                    // Same off-thread bookkeeping as localPath(): the
                    // enforcement walk must not stall this reply handler.
                    QtConcurrent::run([dest] {
                        CacheJanitor::record(dest);
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

void ImageCache::cancelUrl(const QString &url) {
    auto it = m_active.find(url);
    if (it == m_active.end())
        return;
    disconnect(it.value(), nullptr, this, nullptr);
    it.value()->abort();
    it.value()->deleteLater();
    m_active.erase(it);
    // Drop the partial file if one was started.
    QFile::remove(targetPath(url));
}
