#include "thumbloader.h"
#include "cachejanitor.h"
#include "post.h"
#include "remotesource.h"
#include "thumbcommon.h"
#include "webdlbackend.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QImageReader>
#include <QMediaPlayer>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTemporaryFile>
#include <QThread>
#include <QThreadStorage>
#include <QTimer>
#include <QUrl>
#include <QVideoFrame>
#include <QVideoSink>

namespace {
// One persistent QNetworkAccessManager per worker thread: thumbnails
// arrive in bursts, and a throwaway NAM per card meant a fresh DNS +
// TCP + TLS handshake for every image (a browser reuses one HTTP/2
// connection for the whole grid). On a slow VPN that handshake churn is
// the difference between a painted grid and a wall of black cards.
struct ThumbNamCache {
    QHash<QString, QNetworkAccessManager *> nams;
    ~ThumbNamCache() { qDeleteAll(nams); }
};
QThreadStorage<ThumbNamCache *> &thumbNamCache() {
    static QThreadStorage<ThumbNamCache *> storage;
    return storage;
}
QNetworkAccessManager *thumbNam(const QString &proxySpec) {
    ThumbNamCache *cache = thumbNamCache().localData();
    if (!cache) {
        cache = new ThumbNamCache;
        thumbNamCache().setLocalData(cache);
    }
    const auto it = cache->nams.constFind(proxySpec);
    if (it != cache->nams.constEnd())
        return it.value();
    auto *nam = new QNetworkAccessManager;
    const QNetworkProxy proxy = RemotePosts::proxyFromSpec(proxySpec);
    if (proxy.type() != QNetworkProxy::NoProxy)
        nam->setProxy(proxy);
    cache->nams.insert(proxySpec, nam);
    return nam;
}

// One plain-GET attempt for a remote preview. Kept out of run() so a
// transient failure can be retried without duplicating the event-loop
// plumbing.
struct ThumbFetch {
    QByteArray bytes;
    QString contentType;
    int http = 0;
    bool transportError = false; // no HTTP answer: DNS/TCP/TLS/timeout
    bool challenge = false;      // wall markup/JSON instead of media
};
ThumbFetch fetchThumbOnce(ThumbLoader *loader, const QString &key,
                          const QString &path, const QString &referer) {
    ThumbFetch out;
    QNetworkRequest req{QUrl(path)};
    // Inactivity guard: a dead VPN tunnel aborts, a slow 4 MB preview
    // that keeps moving does not.
    req.setTransferTimeout(30000);
    if (!referer.isEmpty())
        req.setRawHeader("Referer", referer.toUtf8());
    RemoteSource::applyCookies(req); // clearance header, if any
    req.setHeader(QNetworkRequest::UserAgentHeader,
                  QStringLiteral("box/0.1 (thumbnail)"));
    req.setRawHeader("Accept", "image/*,*/*;q=0.5");
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply *reply =
        thumbNam(RemotePosts::proxyForUrl(path))->get(req);
    QEventLoop loop;
    QTimer total;
    total.setSingleShot(true);
    total.setInterval(60000); // hard ceiling for one preview
    QObject::connect(&total, &QTimer::timeout, &loop, [&] {
        reply->abort();
        loop.quit();
    });
    QObject::connect(reply, &QNetworkReply::finished, &loop,
                     &QEventLoop::quit);
    int lastPct = -2;
    QElapsedTimer sinceEmit;
    sinceEmit.start();
    QObject::connect(
        reply, &QNetworkReply::downloadProgress, &loop,
        [&](qint64 got, qint64 all) {
            const int pct = all > 0 ? int(got * 100 / all) : -1;
            if (pct == lastPct)
                return; // 1 MB over a fast link must not spam the GUI
            // Throttle repaints to the veil's own animation cadence; the
            // final 100% still lands immediately.
            if (pct != 100 && sinceEmit.elapsed() < 150)
                return;
            lastPct = pct;
            sinceEmit.restart();
            emit loader->thumbnailProgress(key, got, all);
        });
    total.start();
    loop.exec();
    out.http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute)
                   .toInt();
    out.bytes = reply->readAll();
    out.contentType =
        reply->header(QNetworkRequest::ContentTypeHeader).toString();
    out.transportError = reply->error() != QNetworkReply::NoError &&
                         out.http <= 0;
    const QString ctl = out.contentType.trimmed().toLower();
    out.challenge =
        RemotePosts::looksLikeCloudflareWall(out.bytes) ||
        RemotePosts::looksLikeChallengePage(out.bytes) ||
        ctl.startsWith(QStringLiteral("text/")) ||
        ctl.contains(QStringLiteral("json")) ||
        ctl.contains(QStringLiteral("xml"));
    reply->deleteLater();
    return out;
}
} // namespace

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
void ThumbLoader::lowerConcurrency() {
    int n = m_pool->maxThreadCount();
    if (n > 1)
        m_pool->setMaxThreadCount(n - 1);
}
bool ThumbLoader::busy() const {
    QMutexLocker lock(&m_inflightMutex);
    return !m_inflight.isEmpty();
}

void ThumbLoader::setWebBackend(WebDownloadBackend *backend) {
    m_webBackend = backend;
}

void ThumbLoader::request(const QString &key, const QString &path, qint64 mtime,
                          qint64 size, int longSide, const QString &referer,
                          bool persistToDisk, bool nsfw,
                          const QString &siteKey) {
    {
        QMutexLocker lock(&m_inflightMutex);
        if (m_inflight.contains(key))
            return; // scroll spam: this row already has a task queued
        m_inflight.insert(key);
    }
    auto *task = new ThumbTask(this, key, path, mtime, size, longSide, referer,
                               persistToDisk, nsfw, siteKey);
    task->setAutoDelete(true);
    emit thumbnailQueued(key);
    m_pool->start(task);
}

void ThumbLoader::releaseKey(const QString &key) {
    QMutexLocker lock(&m_inflightMutex);
    m_inflight.remove(key);
}

void ThumbLoader::cancel() {
    // Queued-but-unstarted runnables never begin; in-flight ones keep
    // their own (bounded) timeouts. Clears the dedupe set so a later
    // request for the same key is not mistaken for an active task.
    m_pool->clear();
    QMutexLocker lock(&m_inflightMutex);
    m_inflight.clear();
}

ThumbTask::ThumbTask(ThumbLoader *loader, QString key, QString path, qint64 mtime,
                     qint64 size, int longSide, QString referer,
                     bool persistToDisk, bool nsfw, QString siteKey)
    : m_loader(loader), m_key(std::move(key)), m_path(std::move(path)),
      m_mtime(mtime), m_size(size), m_longSide(longSide),
      m_referer(std::move(referer)), m_persistToDisk(persistToDisk),
      m_nsfw(nsfw), m_siteKey(std::move(siteKey)) {}

void ThumbTask::run() {
    emit m_loader->thumbnailStarted(m_key);
    const int num = m_loader->m_aspectNum;
    const int den = m_loader->m_aspectDen;
    const double want = double(num) / double(den);
    const QString ext = QFileInfo(m_path).suffix().toLower();
    auto fail = [this] {
        m_loader->releaseKey(m_key);
        emit m_loader->thumbnailFailed(m_key);
    };

    // Disk cache lookup.
    QString diskPath;
    if (m_persistToDisk && !m_loader->m_cacheDir.isEmpty()) {
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
        const QString host = QUrl(m_path).host().toLower();
        const RemotePosts::SiteQuirk *wq = RemotePosts::quirkForHost(host);
        const bool browserFirst =
            m_loader->m_webBackend && wq && (wq->powWall || wq->browserApi);
        QByteArray bytes;
        bool wallSeen = false;
        auto browserFetch = [&]() -> QByteArray {
            if (!m_loader->m_webBackend)
                return {};
            QTemporaryFile tmp;
            if (!tmp.open())
                return {};
            const QString tmpPath = tmp.fileName();
            tmp.close();
            QString error;
            // 45s: long enough for the capture dialog to be solved while
            // this flight is already navigating, short enough that a
            // cancelled wall does not pin the whole thumbnail pool.
            if (m_loader->m_webBackend->fetchBlocking(
                    m_path, tmpPath, &error, 45000)) {
                QFile f(tmpPath);
                if (f.open(QIODevice::ReadOnly))
                    return f.readAll();
            }
            // A failed browser download means the profile could not pass
            // the wall either: either a challenge page or a human captcha.
            wallSeen = true;
            return {};
        };
        if (browserFirst) {
            emit m_loader->thumbnailProgress(m_key, 0, 0); // no byte feed
            bytes = browserFetch();
        } else {
            // Remote preview/sample URL: plain GET into memory through
            // the thread's persistent NAM (connection reuse).
            ThumbFetch got = fetchThumbOnce(m_loader, m_key, m_path, m_referer);
            const bool transient =
                got.transportError || got.http == 429 || got.http >= 500;
            if (got.bytes.isEmpty() && transient) {
                QThread::msleep(400); // one cheap retry for VPN/TLS hiccups
                got = fetchThumbOnce(m_loader, m_key, m_path, m_referer);
            }
            const int httpStatus = got.http;
            const QByteArray &body = got.bytes;
            const QString ctl = got.contentType.trimmed().toLower();
            const bool httpError = got.transportError || httpStatus >= 400;
            if (!httpError)
                bytes = body;
            if (httpError) {
                // Empty 403/449/503 bodies (CDN challenge without markup)
                // look like plain HTTP errors, but the browser can still
                // pass, so the status alone registers the wall.
                wallSeen = httpStatus == 401 || httpStatus == 403 ||
                           httpStatus == 429 || httpStatus == 449 ||
                           httpStatus == 503 || got.challenge;
            } else if (!body.isEmpty() &&
                       (!RemotePosts::looksLikeMediaBytes(body.left(512)) ||
                        got.challenge)) {
                // 200 but markup/JSON instead of bytes (Cloudflare JS
                // interstitial): never let it decode as a black card.
                bytes.clear();
                wallSeen = true;
            }
            // Only real rate limiting should slow the pool. A VPN timeout
            // used to drop it to one thread permanently, so one hiccup
            // made the rest of the grid look dead.
            if (httpStatus == 429 || httpStatus == 503)
                m_loader->lowerConcurrency();
            if (!httpError && body.isEmpty())
                wallSeen = false; // empty success body: plain failure
            // Cloudflare/captcha hosts get one browser ride too (Danbooru
            // & co. have no powWall quirk but still reject QNAM's TLS
            // fingerprint even with live clearance). Tell the shell right
            // away so the capture pass can start while the browser tries
            // (waiting for its timeout before opening the dialog felt
            // like a frozen grid); MainWindow dedupes per site.
            if (bytes.isEmpty() && wallSeen) {
                emit m_loader->thumbnailWall(m_siteKey, m_path);
                emit m_loader->thumbnailProgress(m_key, 0, 0); // browser ride
                bytes = browserFetch();
            }
        }
        if (bytes.isEmpty()) {
            if (wallSeen && browserFirst)
                emit m_loader->thumbnailWall(m_siteKey, m_path);
            fail();
            return;
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
            fail();
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
            fail();
            return;
        }
    }

    if (!diskPath.isEmpty()) {
        img.save(diskPath, "JPEG", 82); // best effort
        CacheJanitor::record(diskPath, m_nsfw, m_siteKey);
        CacheJanitor::enforce();
    }
    m_loader->releaseKey(m_key);
    emit m_loader->thumbnailReady(m_key, img);
}

// ── VideoThumbGrabber ────────────────────────────────────────────

VideoThumbGrabber::VideoThumbGrabber(QObject *parent) : QObject(parent) {
    m_player = new QMediaPlayer(this);
    // No audio output: silent by construction. The video sink is created
    // fresh per job in pump() (see below).
    m_timeout = new QTimer(this);
    m_timeout->setSingleShot(true);
    m_timeout->setInterval(8000);
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
    // "|v2": per-job sinks fixed late frames landing on the wrong video;
    // re-key once so thumbnails poisoned by that bug are regenerated.
    const QByteArray id = QStringLiteral("%1|%2|%3|%4x%5|%6|v2")
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
                                qint64 size, int longSide, bool persistToDisk,
                                bool nsfw, const QString &siteKey) {
    // Disk cache first: same store the pool loader uses.
    const QString dp = persistToDisk
                           ? videoDiskPath(m_cacheDir, path, mtime, size,
                                           m_aspectNum, m_aspectDen, longSide)
                           : QString();
    if (!dp.isEmpty()) {
        QImage cached;
        if (cached.load(dp)) {
            CacheJanitor::hit(dp);
            emit thumbnailReady(key, cached);
            return;
        }
    }
    if (m_queued.contains(key))
        return;
    m_queued.insert(key);
    m_queue.append({key, path, mtime, size, longSide, persistToDisk, nsfw,
                    siteKey});
    emit thumbnailQueued(key);
    pump();
}

void VideoThumbGrabber::pump() {
    if (m_busy || m_queue.isEmpty())
        return;
    m_busy = true;
    m_current = m_queue.takeFirst();
    emit thumbnailStarted(m_current.key);
    m_want = double(m_aspectNum) / double(m_aspectDen);
    m_diskPath =
        m_current.persistToDisk
            ? videoDiskPath(m_cacheDir, m_current.path, m_current.mtime,
                            m_current.size, m_aspectNum, m_aspectDen,
                            m_current.longSide)
            : QString();
    // Fresh sink per job: frames decoded for the previous video but
    // delivered late must never be attributed to this job (a wrong
    // thumbnail here is then frozen by the disk cache). A late emission
    // targets the retired sink, whose sender check below drops it.
    if (m_sink)
        m_sink->deleteLater();
    m_sink = new QVideoSink(this);
    connect(m_sink, &QVideoSink::videoFrameChanged, this,
            [this](const QVideoFrame &f) {
                if (!m_busy || sender() != m_sink)
                    return;
                QVideoFrame copy(f);
                const QImage conv = copy.toImage();
                if (!conv.isNull())
                    finishCurrent(conv.convertToFormat(QImage::Format_RGB32));
            });
    m_player->setVideoOutput(m_sink);
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
    if (!m_diskPath.isEmpty()) {
        out.save(m_diskPath, "JPEG", 82); // best effort
        CacheJanitor::record(m_diskPath, m_current.nsfw, m_current.siteKey);
        CacheJanitor::enforce();
    }
    emit thumbnailReady(m_current.key, out);
    pump();
}

void VideoThumbGrabber::cancel() {
    m_queue.clear();
    m_queued.clear();
    m_timeout->stop();
    m_player->stop();
    m_busy = false;
}
