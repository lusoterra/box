#include "thumbloader.h"
#include "cachejanitor.h"
#include "post.h"
#include "remotesource.h"
#include "thumbcommon.h"
#include "webdlbackend.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QMediaPlayer>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTemporaryFile>
#include <QTimer>
#include <QUrl>
#include <QVideoFrame>
#include <QVideoSink>

ThumbLoader::ThumbLoader(QObject *parent) : QObject(parent) {
    m_pool = new QThreadPool(this);
    m_pool->setMaxThreadCount(4);
}

void ThumbLoader::setAspect(int num, int den) {
    m_aspectNum = qMax(1, num);
    m_aspectDen = qMax(1, den);
}

void ThumbLoader::setCacheDir(const QString &dir) {
    m_cacheDir = dir;
    if (!dir.isEmpty())
        QDir().mkpath(dir);
}

void ThumbLoader::setMaxThreads(int n) {
    m_pool->setMaxThreadCount(n);
}

void ThumbLoader::setWebBackend(WebDownloadBackend *backend) {
    m_webBackend = backend;
}

void ThumbLoader::request(const QString &key, const QString &path, qint64 mtime,
                          qint64 size, int longSide, const QString &referer) {
    {
        QMutexLocker lock(&m_inflightMutex);
        if (m_inflight.contains(key))
            return; // scroll spam: this row already has a task queued
        m_inflight.insert(key);
    }
    auto *task = new ThumbTask(this, key, path, mtime, size, longSide, referer);
    task->setAutoDelete(true);
    m_pool->start(task);
}

void ThumbLoader::releaseKey(const QString &key) {
    QMutexLocker lock(&m_inflightMutex);
    m_inflight.remove(key);
}

ThumbTask::ThumbTask(ThumbLoader *loader, QString key, QString path, qint64 mtime,
                     qint64 size, int longSide, QString referer)
    : m_loader(loader), m_key(std::move(key)), m_path(std::move(path)),
      m_mtime(mtime), m_size(size), m_longSide(longSide),
      m_referer(std::move(referer)) {}

void ThumbTask::run() {
    const int num = m_loader->m_aspectNum;
    const int den = m_loader->m_aspectDen;
    const double want = double(num) / double(den);
    const QString ext = QFileInfo(m_path).suffix().toLower();

    // Disk cache lookup.
    QString diskPath;
    if (!m_loader->m_cacheDir.isEmpty()) {
        const QByteArray id = QStringLiteral("%1|%2|%3|%4x%5|%6")
                                  .arg(m_path)
                                  .arg(m_mtime)
                                  .arg(m_size)
                                  .arg(num)
                                  .arg(den)
                                  .arg(m_longSide)
                                  .toUtf8();
        diskPath = m_loader->m_cacheDir + u'/' +
                   QString::fromLatin1(
                       QCryptographicHash::hash(id, QCryptographicHash::Sha1).toHex()) +
                   QStringLiteral(".jpg");
        QImage cached;
        if (cached.load(diskPath)) {
            CacheJanitor::hit(diskPath);
            m_loader->releaseKey(m_key);
            emit m_loader->thumbnailReady(m_key, cached);
            return;
        }
    }

    QImage img;
    if (m_path.startsWith(QStringLiteral("http"))) {
        // Fingerprint-walled hosts ride the browser stack (plain
        // requests get wall pages instead of bytes there).
        const RemotePosts::SiteQuirk *wq = RemotePosts::quirkForHost(
            QUrl(m_path).host().toLower());
        QByteArray bytes;
        if (m_loader->m_webBackend && wq && wq->powWall) {
            QTemporaryFile tmp;
            if (tmp.open()) {
                const QString tmpPath = tmp.fileName();
                tmp.close();
                QString error;
                if (m_loader->m_webBackend->fetchBlocking(
                        m_path, tmpPath, &error, 90000)) {
                    QFile f(tmpPath);
                    if (f.open(QIODevice::ReadOnly))
                        bytes = f.readAll();
                }
            }
        } else {
            // Remote preview/sample URL: plain GET into memory.
            QNetworkAccessManager nam;
            const QNetworkProxy thumbProxy = RemotePosts::proxyFromSpec(
                RemotePosts::proxyForUrl(m_path));
            if (thumbProxy.type() != QNetworkProxy::NoProxy)
                nam.setProxy(thumbProxy);
            QEventLoop loop;
            QTimer timer;
            timer.setSingleShot(true);
            timer.setInterval(20000);
            QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
            QNetworkRequest req{QUrl(m_path)};
            if (!m_referer.isEmpty())
                req.setRawHeader("Referer", m_referer.toUtf8());
            RemoteSource::applyCookies(req); // clearance header, if any
            req.setHeader(QNetworkRequest::UserAgentHeader,
                          QStringLiteral("booru-explorer/0.1 (thumbnail)"));
            req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                             QNetworkRequest::NoLessSafeRedirectPolicy);
            QNetworkReply *reply = nam.get(req);
            QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
            timer.start();
            loop.exec();
            if (reply->error() == QNetworkReply::NoError)
                bytes = reply->readAll();
            reply->deleteLater();
        }
        // Bounded decode: hostile dimensions must not balloon memory.
        // QImageReader downscales while decoding, like local files below.
        if (!bytes.isEmpty()) {
            QBuffer buf(&bytes);
            buf.open(QIODevice::ReadOnly);
            QImageReader reader(&buf);
            reader.setAutoTransform(true);
            const QSize full = reader.size();
            if (full.isValid() &&
                (full.width() > m_longSide || full.height() > m_longSide)) {
                const double k = qMin(double(m_longSide) / full.width(),
                                      double(m_longSide) / full.height());
                reader.setScaledSize(QSize(qMax(1, int(full.width() * k)),
                                           qMax(1, int(full.height() * k))));
            }
            img = reader.read();
        }
        img = ThumbCommon::cropScale(img, want, m_longSide);
        if (img.isNull()) {
            m_loader->releaseKey(m_key);
            return;
        }
    } else if (isVideoExt(ext)) {
        // Videos are handled by VideoThumbGrabber in the GUI thread
        // (QMediaPlayer is unreliable in pool threads). If one ever lands
        // here, emit the placeholder so the grid never hangs on gray.
        img = ThumbCommon::videoPlaceholder(
            ext, want, m_longSide);
    } else {
        QImageReader reader(m_path);
        reader.setAutoTransform(true);
        const QSize full = reader.size();
        if (full.isValid() && (full.width() > m_longSide || full.height() > m_longSide)) {
            const double k =
                qMin(double(m_longSide) / full.width(), double(m_longSide) / full.height());
            reader.setScaledSize(
                QSize(qMax(1, int(full.width() * k)), qMax(1, int(full.height() * k))));
        }
        img = ThumbCommon::cropScale(reader.read(), want, m_longSide);
        if (img.isNull()) {
            m_loader->releaseKey(m_key);
            return;
        }
    }

    if (!diskPath.isEmpty()) {
        img.save(diskPath, "JPEG", 82); // best effort
        CacheJanitor::record(diskPath);
        CacheJanitor::enforce();
    }
    m_loader->releaseKey(m_key);
    emit m_loader->thumbnailReady(m_key, img);
}

// ── VideoThumbGrabber ────────────────────────────────────────────

VideoThumbGrabber::VideoThumbGrabber(QObject *parent) : QObject(parent) {
    m_player = new QMediaPlayer(this);
    m_sink = new QVideoSink(this);
    m_player->setVideoOutput(m_sink); // no audio output: silent by construction
    m_timeout = new QTimer(this);
    m_timeout->setSingleShot(true);
    m_timeout->setInterval(8000);
    connect(m_sink, &QVideoSink::videoFrameChanged, this,
            [this](const QVideoFrame &f) {
                if (!m_busy)
                    return;
                QVideoFrame copy(f);
                const QImage conv = copy.toImage();
                if (!conv.isNull())
                    finishCurrent(conv.convertToFormat(QImage::Format_RGB32));
            });
    connect(m_timeout, &QTimer::timeout, this, [this] {
        if (!m_busy)
            return;
        const QString ext = QFileInfo(m_current.path).suffix().toLower();
        finishCurrent(ThumbCommon::videoPlaceholder(ext, m_want, m_current.longSide));
    });
    connect(m_player, &QMediaPlayer::errorOccurred, this, [this] {
        if (!m_busy)
            return;
        const QString ext = QFileInfo(m_current.path).suffix().toLower();
        finishCurrent(ThumbCommon::videoPlaceholder(ext, m_want, m_current.longSide));
    });
}

void VideoThumbGrabber::setAspect(int num, int den) {
    m_aspectNum = qMax(1, num);
    m_aspectDen = qMax(1, den);
}

void VideoThumbGrabber::setCacheDir(const QString &dir) {
    m_cacheDir = dir;
    if (!dir.isEmpty())
        QDir().mkpath(dir);
}

QString videoDiskPath(const QString &cacheDir, const QString &path, qint64 mtime,
                      qint64 size, int num, int den, int longSide) {
    if (cacheDir.isEmpty())
        return {};
    const QByteArray id = QStringLiteral("%1|%2|%3|%4x%5|%6")
                              .arg(path)
                              .arg(mtime)
                              .arg(size)
                              .arg(num)
                              .arg(den)
                              .arg(longSide)
                              .toUtf8();
    return cacheDir + u'/' +
           QString::fromLatin1(
               QCryptographicHash::hash(id, QCryptographicHash::Sha1).toHex()) +
           QStringLiteral(".jpg");
}

void VideoThumbGrabber::request(const QString &key, const QString &path, qint64 mtime,
                                qint64 size, int longSide) {
    // Disk cache first: same store the pool loader uses.
    const QString dp =
        videoDiskPath(m_cacheDir, path, mtime, size, m_aspectNum, m_aspectDen, longSide);
    if (!dp.isEmpty()) {
        QImage cached;
        if (cached.load(dp)) {
            emit thumbnailReady(key, cached);
            return;
        }
    }
    if (m_queued.contains(key))
        return;
    m_queued.insert(key);
    m_queue.append({key, path, mtime, size, longSide});
    pump();
}

void VideoThumbGrabber::pump() {
    if (m_busy || m_queue.isEmpty())
        return;
    m_busy = true;
    m_current = m_queue.takeFirst();
    m_want = double(m_aspectNum) / double(m_aspectDen);
    m_diskPath = videoDiskPath(m_cacheDir, m_current.path, m_current.mtime,
                               m_current.size, m_aspectNum, m_aspectDen,
                               m_current.longSide);
    m_player->setSource(m_current.path.startsWith(QStringLiteral("http"))
                            ? QUrl(m_current.path)
                            : QUrl::fromLocalFile(m_current.path));
    m_player->play();
    m_timeout->start();
}

void VideoThumbGrabber::finishCurrent(const QImage &img) {
    m_timeout->stop();
    m_player->stop();
    m_queued.remove(m_current.key);
    m_busy = false;
    QImage out = ThumbCommon::cropScale(img, m_want, m_current.longSide);
    if (!m_diskPath.isEmpty())
        out.save(m_diskPath, "JPEG", 82); // best effort
    emit thumbnailReady(m_current.key, out);
    pump();
}
