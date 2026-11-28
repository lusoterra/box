#include "downloadmanager.h"
#include "downloadqueue.h"
#include "imagecache.h"
#include "filehash.h"
#include "metaparser.h"
#include "postmeta.h"
#include "progressbar.h"
#include "remotesource.h"
#include "ugoioconvert.h"
#include "uicons.h"
#include "webdlbackend.h"

#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QListWidget>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPushButton>
#include <QRegularExpression>
#include <QSet>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

DownloadManager::DownloadManager(QObject *parent)
    : QObject(parent), m_nam(new QNetworkAccessManager(this)) {
    m_notesFetcher = new PostMetaFetcher(this);
    connect(m_notesFetcher, &PostMetaFetcher::done, this,
            [this](const QString &, qint64, const QVector<PostNote> &notes,
                   const QVector<PostComment> &, const QVector<PostComment> &,
                   bool ok, const QString &) {
                if (ok && !m_notesActiveSidecar.isEmpty())
                    writeSidecarNotes(m_notesActiveSidecar, notes);
                m_notesActiveSidecar.clear();
                pumpNotesCache();
            });
}

DownloadManager::~DownloadManager() {
    delete m_store;
    m_store = nullptr;
}

QNetworkAccessManager *DownloadManager::namForProxy(const QString &proxySpec) {
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

namespace {
QString proxySpecForJob(const DownloadJob &job) {
    // Stamped entry first (grid selections), host match as fallback.
    if (!job.post.siteKey.isEmpty()) {
        if (const BooruSite *site = RemoteSource::siteFor(job.post.siteKey)) {
            if (!site->proxy.trimmed().isEmpty())
                return site->proxy.trimmed();
        }
    }
    return RemotePosts::proxyForUrl(job.post.fileUrl);
}
} // namespace

namespace {
// 32-hex file stems double as content hashes on most boorus (Gelbooru,
// Safebooru, Danbooru CDN...). Same rule as LosnImport::md5FromFileUrl.
QString md5Stem(const QString &fileUrl) {
    const QString stem = QUrl(fileUrl).fileName().split(u'.').first();
    static const QRegularExpression hex32(QStringLiteral("^[0-9a-f]{32}$"));
    return hex32.match(stem).hasMatch() ? stem : QString();
}

// Move a completed file onto its final download path. A rename keeps the
// bytes on the same filesystem (the cache -> downloads case: instant), with
// a copy fallback for cross-device destinations. Never leaves both copies.
bool moveFileOverwrite(const QString &src, const QString &dest) {
    if (src.isEmpty() || dest.isEmpty())
        return false;
    if (QFileInfo(src).absoluteFilePath() == QFileInfo(dest).absoluteFilePath())
        return QFile::exists(src);
    QDir().mkpath(QFileInfo(dest).absolutePath());
    if (QFile::exists(dest) && !QFile::remove(dest))
        return false;
    if (QFile::rename(src, dest))
        return true;
    if (!QFile::copy(src, dest))
        return false;
    QFile::remove(src);
    return true;
}

QString partPathFor(const DownloadJob &job) {
    return job.filePath + QStringLiteral(".part");
}
} // namespace

void DownloadManager::setImageCache(ImageCache *cache) {
    if (m_imgCache == cache)
        return;
    if (m_imgCache) {
        disconnect(m_imgCache, nullptr, this, nullptr);
        for (const QString &url : std::as_const(m_cachePinned))
            m_imgCache->unpin(url);
        m_cachePinned.clear();
    }
    m_imgCache = cache;
    if (!m_imgCache)
        return;
    connect(m_imgCache, &ImageCache::ready, this,
            &DownloadManager::onCacheReady);
    connect(m_imgCache, &ImageCache::failed, this,
            &DownloadManager::onCacheFailed);
}

void DownloadManager::setWebBackend(WebDownloadBackend *backend) {
    if (m_webBackend == backend)
        return;
    m_webBackend = backend;
    if (!m_webBackend)
        return;
    connect(m_webBackend, &WebDownloadBackend::progress, this,
            &DownloadManager::onWebProgress);
    connect(m_webBackend, &WebDownloadBackend::finished, this,
            &DownloadManager::onWebFinished);
}

DownloadManager::Flight *DownloadManager::findWebFlight(qint64 webId) {
    for (Flight *f : m_flights) {
        if (f->webId == webId)
            return f;
    }
    return nullptr;
}

namespace {
// Sidecar enrichment: the raw API object wins, but favourited rows
// re-listed from the DB carry no API object at all. Backfill the
// gallery-dl-style fields from the parsed post so every sidecar is a
// complete record (rescan, relink and the file name all read these).
void enrichSidecarRaw(QJsonObject &raw, const Post &p) {
    auto putNum = [&](const char *k, qint64 v) {
        if (v == 0)
            return;
        const QString key = QString::fromLatin1(k);
        if (raw.value(key).toVariant().toLongLong() == 0)
            raw[key] = v;
    };
    auto putStr = [&](const char *k, const QString &v) {
        if (v.isEmpty())
            return;
        const QString key = QString::fromLatin1(k);
        if (raw.value(key).isNull() || raw.value(key).toString().isEmpty())
            raw[key] = v;
    };
    putNum("id", p.id);
    putNum("score", p.score);
    putNum("width", p.width);
    putNum("height", p.height);
    putStr("file_ext", p.ext);
    putStr("file_url", p.fileUrl);
    putStr("sample_url", p.sampleUrl);
    putStr("preview_url", p.previewUrl);
    putStr("post_url", p.postUrl);
    putStr("rating", p.rating);
    putStr("source", p.source);
    putStr("title", p.title);
    putStr("date", p.date);
    putStr("tags", p.tags);
    putStr("tags_artist", p.tagsArtist);
    putStr("tags_character", p.tagsCharacter);
    putStr("tags_copyright", p.tagsCopyright);
    putStr("tags_general", p.tagsGeneral);
    putStr("tags_metadata", p.tagsMeta);
}
} // namespace

QStringList DownloadManager::sidecarPathsFor(const QString &absFile) {
    const QFileInfo fi(absFile);
    QStringList out;
    out.append(fi.absolutePath() + u'/' +
               fi.completeBaseName() + QStringLiteral(".json"));
    out.append(absFile + QStringLiteral(".json"));
    QStringList dedup;
    for (const QString &p : out) {
        if (!dedup.contains(p) && p != absFile)
            dedup.append(p);
    }
    return dedup;
}

QList<DownloadJob> DownloadManager::plan(const QVector<Post> &posts,
                                         const QString &destDir) {
    QList<DownloadJob> jobs;
    QSet<QString> used; // same batch must never reuse a path: the second
                        // job would silently overwrite the first
    for (const Post &p : posts) {
        // http(s) only: a rogue API object pointing fileUrl at file:///
        // (or javascript:/data:) must never become a request or a file.
        if (!p.isRemote || p.fileUrl.isEmpty() ||
            !RemotePosts::isHttpUrl(p.fileUrl))
            continue; // local files + login-walled entries are skipped
        // Favourites re-listed from the DB carry no API object, so a raw
        // name lookup yields Site_0 for every row and the whole batch
        // collapses onto one path. Rebuild just enough naming info from
        // the post itself; real API objects pass through untouched.
        QJsonObject raw = p.rawJson;
        if (raw.value(QStringLiteral("md5")).toString().isEmpty() &&
            raw.value(QStringLiteral("checksumMD5")).toString().isEmpty())
            raw[QStringLiteral("md5")] = md5Stem(p.fileUrl);
        if (raw.value(QStringLiteral("id")).toVariant().toLongLong() == 0 &&
            p.id != 0)
            raw[QStringLiteral("id")] = p.id;
        if (raw.value(QStringLiteral("file_ext")).toString().isEmpty()) {
            if (!p.ext.isEmpty())
                raw[QStringLiteral("file_ext")] = p.ext;
            else
                raw[QStringLiteral("file_url")] = p.fileUrl;
        }
        enrichSidecarRaw(raw, p);
        // Strays keep their marker even when re-pulled from a favourites
        // row (the row carries no rawJson): rescan then parses the flat
        // tags/stray record instead of a generic gallery-dl sidecar.
        if (p.siteName == QStringLiteral("_Stray"))
            raw[QStringLiteral("stray")] = true;
        const QString tag = RemotePosts::fsTagForPost(p);
        const QString dir =
            destDir + u'/' + RemotePosts::safeFolderSegment(tag);
        QString name = RemotePosts::downloadFileName(tag, raw);
        if (used.contains(dir + u'/' + name)) {
            const qsizetype dot = name.lastIndexOf(u'.');
            const QString stem = name.left(dot), ext = name.mid(dot);
            int n = 2;
            while (used.contains(dir + u'/' + stem + u'_' +
                                 QString::number(n) + ext))
                ++n;
            name = stem + u'_' + QString::number(n) + ext;
        }
        DownloadJob job;
        job.post = p;
        job.post.rawJson = raw; // sidecar keeps id/md5/file info, not {}
        job.fileName = name;
        job.filePath = dir + u'/' + name;
        const qsizetype dot = name.lastIndexOf(u'.');
        job.jsonPath = dir + u'/' + name.left(dot) + QStringLiteral(".json");
        used.insert(job.filePath);
        jobs.append(job);
    }
    return jobs;
}

QList<DownloadJob> DownloadManager::dedupeJobs(const QList<DownloadJob> &jobs,
                                               QList<DownloadJob> *skipped) {
    QList<DownloadJob> unique;
    QSet<QString> used;
    for (const DownloadJob &j : jobs) {
        if (used.contains(j.filePath)) {
            if (skipped)
                skipped->append(j);
            continue;
        }
        used.insert(j.filePath);
        unique.append(j);
    }
    return unique;
}

void DownloadManager::persistAccepted(const QList<DownloadJob> &jobs) {
    if (m_store)
        m_store->add(jobs);
}

void DownloadManager::forgetPersisted(const QString &url) {
    if (m_store && !url.isEmpty())
        m_store->removeUrl(url);
}

void DownloadManager::enqueue(const QList<DownloadJob> &jobsIn) {
    // A batch that arrives while a previous batch is still running is
    // appended instead of cancelling it: browsing to another post and
    // pressing Download must never abort the file already in flight.
    const bool fresh = !m_active && m_queue.isEmpty() && m_flights.isEmpty() &&
                       m_cacheWait.isEmpty();
    if (fresh) {
        m_summary = DownloadSummary();
        m_done = 0;
        m_total = 0;
        if (m_store)
            m_store->clear(); // new session starts from this batch only
    }

    QSet<QString> knownPaths;
    QSet<QString> knownUrls;
    for (const DownloadJob &j : std::as_const(m_queue)) {
        knownPaths.insert(j.filePath);
        knownUrls.insert(j.post.fileUrl);
    }
    for (const Flight *f : std::as_const(m_flights)) {
        knownPaths.insert(f->job.filePath);
        knownUrls.insert(f->job.post.fileUrl);
    }
    for (const DownloadJob &j : std::as_const(m_cacheWait)) {
        knownPaths.insert(j.filePath);
        knownUrls.insert(j.post.fileUrl);
    }

    QList<DownloadJob> skipped;
    QList<DownloadJob> duplicates;
    const QList<DownloadJob> jobs = dedupeJobs(jobsIn, &duplicates);
    auto skipOne = [&](const DownloadJob &j) {
        // Same bytes, same target from a duplicate DB row: one fetch is
        // enough. Record the path so callers linking rows still see it.
        m_summary.skipped++;
        m_summary.downloaded.append(j.post);
        m_summary.finalPaths.append({j.post.fileUrl, j.filePath});
        ++m_done; // skipped rows still advance the bar
        skipped.append(j);
        forgetPersisted(j.post.fileUrl);
    };
    for (const DownloadJob &j : std::as_const(duplicates))
        skipOne(j);
    QList<DownloadJob> accepted;
    for (const DownloadJob &j : jobs) {
        if (knownPaths.contains(j.filePath) ||
            knownUrls.contains(j.post.fileUrl))
            continue; // already queued/in flight: never start a second fetch
        if (QFile::exists(j.filePath)) {
            // Already on disk still counts as downloaded for the
            // favourite coupling (otherwise re-downloads never get
            // favourited/linked and Downloads/Favourites diverge).
            skipOne(j);
            continue;
        }
        QFile::remove(partPathFor(j)); // stale interrupted attempt
        accepted.append(j);
        knownPaths.insert(j.filePath);
        knownUrls.insert(j.post.fileUrl);
    }

    m_queue.append(accepted);
    m_total += static_cast<int>(accepted.size() + skipped.size());
    if (fresh)
        emit batchStarted(m_total);
    if (m_queue.isEmpty() && m_flights.isEmpty() && m_cacheWait.isEmpty()) {
        m_active = false;
        emit finished(m_summary);
        return;
    }
    m_convertCancel.storeRelaxed(0); // fresh work clears any prior abort
    m_active = true;
    persistAccepted(accepted);
    emit progress(m_done, m_total, QString());
    for (const DownloadJob &j : std::as_const(skipped))
        emit fileSkipped(j.fileName);
    pump();
}

void DownloadManager::cancel() {
    const bool wasActive = m_active;
    m_convertCancel.storeRelaxed(1); // abort any in-flight ugoira encode
    QSet<QString> touchedDirs;
    for (Flight *f : std::as_const(m_flights)) {
        if (f->webId >= 0 && m_webBackend)
            m_webBackend->cancel(f->webId); // backend drops partial files
        if (f->reply) {
            disconnect(f->reply, nullptr, this, nullptr);
            f->reply->abort();
            f->reply->deleteLater();
        }
        if (f->file) {
            f->file->close();
            delete f->file;
        }
        QFile::remove(f->partPath); // interrupted bytes, never the final
        touchedDirs.insert(QFileInfo(f->job.filePath).absolutePath());
        delete f;
    }
    m_flights.clear();
    for (const DownloadJob &j : std::as_const(m_cacheWait)) {
        QFile::remove(partPathFor(j));
        m_cachePinned.remove(j.post.fileUrl);
        if (m_imgCache) {
            m_imgCache->unpin(j.post.fileUrl);
            m_imgCache->cancelUrl(j.post.fileUrl); // explicit abort
        }
        touchedDirs.insert(QFileInfo(j.filePath).absolutePath());
    }
    m_cacheWait.clear();
    for (const DownloadJob &j : std::as_const(m_queue))
        QFile::remove(partPathFor(j));
    m_queue.clear();
    // Aborted attempts leave no empty host folder once nothing needs it.
    for (const QString &dir : std::as_const(touchedDirs))
        removeEmptyDir(dir);
    if (m_store)
        m_store->clear(); // explicit abort forgets the batch
    m_active = false;
    m_done = 0;
    m_total = 0;
    m_summary = DownloadSummary();
    if (wasActive)
        emit cancelled();
}

void DownloadManager::stopAfterCurrent() {
    for (const DownloadJob &j : std::as_const(m_queue))
        QFile::remove(partPathFor(j));
    m_queue.clear();
    // Flights and cache waits are "current": let them land. Their journal
    // rows are removed on completion, queued rows stay for the next launch.
    if (m_active && m_flights.isEmpty() && m_cacheWait.isEmpty()) {
        // Deferred: the caller is inside a modal quit prompt and will
        // close the window from the finished handler.
        QTimer::singleShot(0, this, [this] {
            if (m_active && m_flights.isEmpty() && m_cacheWait.isEmpty() &&
                m_queue.isEmpty()) {
                m_active = false;
                emit finished(m_summary);
            }
        });
    }
}

void DownloadManager::shutdown() {
    m_convertCancel.storeRelaxed(1);
    for (Flight *f : std::as_const(m_flights)) {
        if (f->webId >= 0 && m_webBackend)
            m_webBackend->cancel(f->webId);
        if (f->reply) {
            disconnect(f->reply, nullptr, this, nullptr);
            f->reply->abort();
            f->reply->deleteLater();
        }
        if (f->file) {
            f->file->close();
            delete f->file;
        }
        QFile::remove(f->partPath);
        delete f;
    }
    m_flights.clear();
    for (const QString &url : std::as_const(m_cachePinned)) {
        if (m_imgCache)
            m_imgCache->unpin(url);
    }
    m_cachePinned.clear();
    m_cacheWait.clear();
    m_queue.clear();
    m_active = false;
    // The journal keeps every unfinished job on purpose: the next launch
    // offers to resume it.
}

void DownloadManager::setQueueStorePath(const QString &path) {
    delete m_store;
    m_store = path.isEmpty() ? nullptr : new DownloadQueueStore(path);
}

QList<DownloadJob> DownloadManager::persistedJobs() const {
    return m_store ? m_store->load() : QList<DownloadJob>();
}

void DownloadManager::resumePersistedQueue() {
    const QList<DownloadJob> jobs = persistedJobs();
    if (!jobs.isEmpty())
        enqueue(jobs);
}

void DownloadManager::discardPersistedQueue() {
    const QList<DownloadJob> jobs = persistedJobs();
    for (const DownloadJob &j : jobs)
        QFile::remove(partPathFor(j));
    if (m_store)
        m_store->clear();
}

int DownloadManager::pendingCount() const {
    return static_cast<int>(m_queue.size() + m_flights.size() +
                            m_cacheWait.size());
}

void DownloadManager::cancelUrls(const QSet<QString> &fileUrls) {
    if (fileUrls.isEmpty())
        return;
    const int before = pendingCount();
    for (const QString &url : fileUrls)
        forgetPersisted(url);
    QSet<QString> touchedDirs;
    m_queue.erase(std::remove_if(m_queue.begin(), m_queue.end(),
                                 [&](const DownloadJob &j) {
                                     if (!fileUrls.contains(j.post.fileUrl))
                                         return false;
                                     QFile::remove(partPathFor(j));
                                     touchedDirs.insert(
                                         QFileInfo(j.filePath).absolutePath());
                                     return true;
                                 }),
                  m_queue.end());
    QList<DownloadJob> deadWaits;
    for (const DownloadJob &j : std::as_const(m_cacheWait)) {
        if (!fileUrls.contains(j.post.fileUrl))
            continue;
        deadWaits.append(j);
        if (m_imgCache) {
            m_imgCache->unpin(j.post.fileUrl);
            m_imgCache->cancelUrl(j.post.fileUrl);
        }
        m_cachePinned.remove(j.post.fileUrl);
        touchedDirs.insert(QFileInfo(j.filePath).absolutePath());
    }
    m_cacheWait.erase(std::remove_if(m_cacheWait.begin(), m_cacheWait.end(),
                                     [&](const DownloadJob &j) {
                                         return fileUrls.contains(
                                             j.post.fileUrl);
                                     }),
                      m_cacheWait.end());
    for (const DownloadJob &j : std::as_const(deadWaits))
        emit fileFinished(j.fileName, false, tr("Cancelled"));
    QList<Flight *> dead;
    for (Flight *f : std::as_const(m_flights)) {
        if (fileUrls.contains(f->job.post.fileUrl))
            dead.append(f);
    }
    for (Flight *f : dead) {
        m_flights.removeAll(f);
        if (f->webId >= 0 && m_webBackend)
            m_webBackend->cancel(f->webId); // backend drops partial files
        if (f->reply) {
            disconnect(f->reply, nullptr, this, nullptr);
            f->reply->abort();
            f->reply->deleteLater();
        }
        if (f->file) {
            f->file->close();
            delete f->file;
        }
        QFile::remove(f->partPath); // interrupted bytes, never the final
        touchedDirs.insert(QFileInfo(f->job.filePath).absolutePath());
        emit fileFinished(f->job.fileName, false, tr("Cancelled"));
        delete f;
    }
    for (const QString &dir : std::as_const(touchedDirs))
        removeEmptyDir(dir);
    m_total = qMax(0, m_total - (before - pendingCount()));
    if (m_queue.isEmpty() && m_flights.isEmpty() && m_cacheWait.isEmpty())
        m_active = false;
    else
        pump(); // fill freed flight slots
}

bool DownloadManager::dirHasPendingJob(const QString &dirPath) const {
    const QString dir = QDir::cleanPath(dirPath);
    auto same = [&dir](const DownloadJob &j) {
        return QDir::cleanPath(QFileInfo(j.filePath).absolutePath()) == dir;
    };
    for (const DownloadJob &j : m_queue)
        if (same(j))
            return true;
    for (const DownloadJob &j : m_cacheWait)
        if (same(j))
            return true;
    for (const Flight *f : m_flights)
        if (f && same(f->job))
            return true;
    return false;
}

void DownloadManager::removeEmptyDir(const QString &dirPath) {
    if (dirPath.isEmpty() || dirHasPendingJob(dirPath))
        return;
    QDir d(dirPath);
    if (d.exists() && d.isEmpty())
        QDir().rmdir(dirPath); // never removes a populated folder
}

void DownloadManager::removeEmptyJobDir(const QString &filePath) {
    if (filePath.isEmpty())
        return;
    removeEmptyDir(QFileInfo(filePath).absolutePath());
}

bool DownloadManager::openStagingFile(Flight *flight) {
    if (!flight)
        return false;
    if (flight->file)
        return true;
    if (flight->writeError)
        return false;
    const QString dir = QFileInfo(flight->job.filePath).absolutePath();
    if (!QDir().mkpath(dir)) {
        flight->writeError = true;
        return false;
    }
    auto *file = new QFile(flight->partPath);
    if (!file->open(QIODevice::WriteOnly)) {
        delete file;
        QFile::remove(flight->partPath);
        flight->writeError = true;
        return false;
    }
    flight->file = file;
    return true;
}

void DownloadManager::pump() {
    while (m_active && m_flights.size() < kMaxFlight && !m_queue.isEmpty()) {
        DownloadJob job = m_queue.takeFirst();
        if (m_imgCache && !job.post.fileUrl.isEmpty()) {
            // Already on disk at full resolution (viewed): move the bytes
            // instead of fetching them again. Cache holds complete
            // image/video bodies only (walls never land there).
            const QString have =
                m_imgCache->localPath(job.post.fileUrl, &job.post);
            if (!have.isEmpty()) {
                if (moveFileOverwrite(have, job.filePath)) {
                    // The bytes now live in the download folder: evict the
                    // cache entry + its bookkeeping.
                    m_imgCache->drop(job.post.fileUrl);
                    QString error;
                    const bool ok = completeJob(job, &error);
                    emit fileStarted(job.fileName);
                    emit progress(m_done, m_total, job.fileName);
                    // No repump: the outer loop keeps filling flights, so a
                    // long all-cached run stays flat instead of recursing.
                    finishJob(job, ok, error, false, false);
                    continue;
                }
            } else if (m_imgCache->isFetching(job.post.fileUrl)) {
                // The viewer is still buffering this exact full-res URL.
                // Park the job instead of starting a second request and pin
                // the cache fetch so stepping to another post cannot abort
                // it. onCacheReady() moves the bytes once they land.
                if (!m_cachePinned.contains(job.post.fileUrl)) {
                    m_imgCache->pin(job.post.fileUrl);
                    m_cachePinned.insert(job.post.fileUrl);
                }
                m_cacheWait.append(job);
                emit fileStarted(job.fileName);
                emit progress(m_done, m_total,
                              tr("Waiting for the viewer download: %1")
                                  .arg(job.fileName));
                continue;
            }
        }
        if (m_webBackend && RemotePosts::needsWebDownload(job.post)) {
            // Fingerprint-walled media: the browser stack downloads
            // with the profile's session (plain requests get wall
            // pages instead of bytes).
            job.webTried = true;
            emit fileStarted(job.fileName);
            emit progress(m_done, m_total, job.fileName);
            startWebFlight(job);
            continue;
        }
        const QString part = partPathFor(job);
        QFile::remove(part);
        QNetworkRequest req(QUrl(job.post.fileUrl));
        req.setTransferTimeout(60000);
        const QString referer = RemotePosts::mediaRefererFor(job.post);
        if (!referer.isEmpty())
            req.setRawHeader("Referer", referer.toUtf8());
        RemoteSource::applyCookies(req); // clearance header, if any
        req.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("box/0.1 (batch download)"));
        req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
        // No staging file/dir yet: a wall/login/offline failure must not
        // leave an empty per-site folder in the download tree.
        auto *flight = new Flight{job, nullptr, nullptr, -1, part};
        flight->reply = namForProxy(proxySpecForJob(job))->get(req);
        connect(flight->reply, &QNetworkReply::finished, this,
                &DownloadManager::onFileReply);
        connect(flight->reply, &QIODevice::readyRead, this, [this, flight] {
            if (openStagingFile(flight))
                flight->file->write(flight->reply->readAll());
        });
        connect(flight->reply, &QNetworkReply::downloadProgress, this,
                [this, flight](qint64 received, qint64 total) {
                    emit fileProgress(flight->job.filePath, received, total);
                });
        m_flights.append(flight);
        emit fileStarted(job.fileName);
        emit progress(m_done, m_total, job.fileName);
    }
    if (m_active && m_flights.isEmpty() && m_cacheWait.isEmpty() &&
        m_queue.isEmpty()) {
        m_active = false;
        emit finished(m_summary);
    }
}

void DownloadManager::startWebFlight(const DownloadJob &job) {
    auto *flight = new Flight{job, nullptr, nullptr, -1, partPathFor(job)};
    flight->webId = m_webNextId++;
    m_flights.append(flight);
    QFile::remove(flight->partPath);
    // The backend writes straight to the staging path, so the folder has
    // to exist first; a failed browser pass drops it again in finishJob.
    QDir().mkpath(QFileInfo(job.filePath).absolutePath());
    m_webBackend->start(flight->webId, job.post.fileUrl, flight->partPath);
}

void DownloadManager::onFileReply() {
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    Flight *flight = nullptr;
    for (Flight *f : m_flights) {
        if (f->reply == reply) {
            flight = f;
            break;
        }
    }
    if (!flight) {
        if (reply)
            reply->deleteLater();
        return;
    }
    m_flights.removeAll(flight);
    bool ok = reply->error() == QNetworkReply::NoError;
    QString error;
    // Final paths: the sidecar + media may be renamed below (md5 fixup),
    // so the job copy - not the queued original - is what finishes.
    DownloadJob job = flight->job;
    const QString part = flight->partPath;
    // Cloudflare/captcha CDNs reject QNAM even with a valid clearance
    // (TLS/UA fingerprint). Instead of failing the download, re-run it
    // once through the capture browser that minted the clearance.
    auto retryInBrowser = [&]() -> bool {
        if (!m_webBackend || job.webTried || job.post.fileUrl.isEmpty())
            return false;
        job.webTried = true;
        if (flight->file) {
            flight->file->close();
            delete flight->file;
            flight->file = nullptr;
        }
        QFile::remove(part);
        reply->deleteLater();
        delete flight;
        startWebFlight(job);
        return true;
    };
    // A small body can already be buffered when finished fires without a
    // separately queued readyRead: stage it now instead of calling it empty.
    if (ok && !flight->file && reply->bytesAvailable() > 0 &&
        openStagingFile(flight)) {
        flight->file->write(reply->readAll());
    }
    const bool localFailure = ok && !flight->file;
    if (localFailure) {
        // The staging file is created on first bytes: a reply that never
        // delivered any is either an empty answer or a failed mkdir/open.
        ok = false;
        error = flight->writeError
                    ? tr("Cannot write %1").arg(job.filePath)
                    : tr("Empty response");
    }
    if (ok && flight->file) {
        const QString ct = reply
                               ->header(QNetworkRequest::ContentTypeHeader)
                               .toString()
                               .trimmed()
                               .toLower();
        // Accept image/video/octet-stream content types, but also trust
        // magic bytes: Shimmie2 (and a few CDNs) serve real media with an
        // empty or text/plain Content-Type. A genuine wall/login page
        // still fails the sniff and is named below.
        const bool headerMedia =
            ct.startsWith(QStringLiteral("image/")) ||
            ct.startsWith(QStringLiteral("video/")) ||
            ct.startsWith(QStringLiteral("application/octet-stream")) ||
            ct.startsWith(QStringLiteral("binary/octet-stream")) ||
            ct.startsWith(QStringLiteral("application/zip")) ||
            ct.startsWith(QStringLiteral("application/x-zip"));
        flight->file->write(reply->readAll()); // trailing bytes
        flight->file->close();
        QByteArray head;
        {
            QFile probe(part);
            if (probe.open(QIODevice::ReadOnly))
                head = probe.read(512);
        }
        const bool sniffMedia = RemotePosts::looksLikeMediaBytes(head);
        if (!headerMedia && !sniffMedia) {
            // Wall/login page instead of bytes: never keep it as media.
            // The head names a captcha/wall and points at cookie capture
            // instead of quoting a misleading content type.
            ok = false;
            const bool isWall =
                RemotePosts::looksLikeChallengePage(head) ||
                RemotePosts::looksLikeCloudflareWall(head);
            error = RemotePosts::wallAwareMediaError(
                QUrl(job.post.fileUrl).host(), head,
                reply->header(QNetworkRequest::ContentTypeHeader).toString());
            if (isWall) {
                QFile::remove(part);
                if (retryInBrowser())
                    return;
                reply->deleteLater();
                finishJob(job, false, error, true);
                delete flight->file;
                delete flight;
                return;
            }
        } else {
            // Bytes are only promoted to the final name after the sniff:
            // a crash or abort never leaves a half-written .jpg behind.
            if (!moveFileOverwrite(part, job.filePath)) {
                ok = false;
                error = tr("Cannot write %1").arg(job.filePath);
            } else {
                bool walled = false;
                if (!completeJob(job, &error, &walled))
                    ok = false;
                if (walled) {
                    if (retryInBrowser())
                        return;
                    reply->deleteLater();
                    finishJob(job, ok, error, true);
                    delete flight->file;
                    delete flight;
                    return;
                }
            }
        }
    } else if (!ok && !localFailure) {
        const int http =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        error = http > 0 ? tr("HTTP %1").arg(http)
                         : reply->errorString();
        // Empty 403/449/503 bodies (CDN challenge without markup) look
        // like plain HTTP errors here, but the browser can still pass.
        if (http == 403 || http == 449 || http == 503 ||
            (http <= 0 &&
             reply->error() == QNetworkReply::ContentAccessDenied)) {
            if (retryInBrowser())
                return;
        }
    }
    if (flight->file) {
        flight->file->close();
        delete flight->file;
    }
    // Only the staging name is ever removed here: on success it was
    // already renamed onto job.filePath.
    QFile::remove(part);
    reply->deleteLater();
    finishJob(job, ok, error);
    delete flight;
}

void DownloadManager::onWebProgress(qint64 id, qint64 received, qint64 total) {
    if (Flight *flight = findWebFlight(id))
        emit fileProgress(flight->job.filePath, received, total);
}

void DownloadManager::onWebFinished(qint64 id, bool ok, const QString &errorIn) {
    Flight *flight = findWebFlight(id);
    if (!flight)
        return;
    m_flights.removeAll(flight);
    DownloadJob job = flight->job;
    const QString part = flight->partPath;
    delete flight;
    QString error = errorIn;
    bool walled = false;
    if (ok) {
        // The backend writes the staged .part name; promote it only after
        // it reports success (a failure/cancel never leaves a final file).
        if (!moveFileOverwrite(part, job.filePath)) {
            ok = false;
            error = tr("Cannot write %1").arg(job.filePath);
        } else if (!completeJob(job, &error, &walled)) {
            ok = false;
        }
    }
    if (!ok)
        QFile::remove(part); // backend normally dropped it already
    finishJob(job, ok, error, walled);
    pump();
}

void DownloadManager::onCacheReady(const QString &url, const QString &localPath) {
    if (m_cacheWait.isEmpty())
        return;
    QList<DownloadJob> ready;
    for (const DownloadJob &j : std::as_const(m_cacheWait)) {
        if (j.post.fileUrl == url)
            ready.append(j);
    }
    if (ready.isEmpty())
        return; // a viewer prefetch, not one of our parked jobs
    m_cacheWait.erase(std::remove_if(m_cacheWait.begin(), m_cacheWait.end(),
                                     [&](const DownloadJob &j) {
                                         return j.post.fileUrl == url;
                                     }),
                      m_cacheWait.end());
    if (m_cachePinned.remove(url) && m_imgCache)
        m_imgCache->unpin(url);
    for (DownloadJob &job : ready) {
        if (!moveFileOverwrite(localPath, job.filePath)) {
            // Cache bytes vanished/undecodable mid-move: fall back to a
            // normal network flight instead of failing the download.
            QFile::remove(partPathFor(job));
            m_queue.prepend(job);
            continue;
        }
        if (m_imgCache)
            m_imgCache->drop(url); // bytes moved: forget the cache entry
        QString error;
        const bool ok = completeJob(job, &error);
        emit progress(m_done, m_total, job.fileName);
        finishJob(job, ok, error, false, false);
    }
    pump();
}

void DownloadManager::onCacheFailed(const QString &url, const QString &) {
    if (m_cacheWait.isEmpty())
        return;
    QList<DownloadJob> failed;
    for (const DownloadJob &j : std::as_const(m_cacheWait)) {
        if (j.post.fileUrl == url)
            failed.append(j);
    }
    if (failed.isEmpty())
        return; // a viewer prefetch failure, not a parked job
    m_cacheWait.erase(std::remove_if(m_cacheWait.begin(), m_cacheWait.end(),
                                     [&](const DownloadJob &j) {
                                         return j.post.fileUrl == url;
                                     }),
                      m_cacheWait.end());
    if (m_cachePinned.remove(url) && m_imgCache)
        m_imgCache->unpin(url);
    // The cache request failed (wall/network): run the normal download
    // path, which has its own browser retry for fingerprint walls.
    for (qsizetype i = failed.size() - 1; i >= 0; --i)
        m_queue.prepend(failed.at(i));
    pump();
}

// Ugoira zip -> animated webp/jxl in place: converts, swaps the job onto
// the converted file (fresh md5 + sidecar naming), and removes the zip.
// Returns false with error set on failure or cancel (zip kept on disk).
bool DownloadManager::convertUgoiraZip(DownloadJob &job, QJsonObject &raw,
                                       QString *error) {
    const QString zipPath = job.filePath;
    const QString stem = QFileInfo(zipPath).completeBaseName();
    const QString dir = QFileInfo(zipPath).absolutePath();
    const QString outName = stem + (m_ugoiraFormat == QStringLiteral("jxl")
                                        ? QStringLiteral(".jxl")
                                        : QStringLiteral(".webp"));
    const QString outExt = QFileInfo(outName).suffix();
    QString outPath = dir + u'/' + outName;
    if (QFile::exists(outPath)) {
        int n = 2;
        while (QFile::exists(dir + u'/' + stem + u'_' + QString::number(n) +
                              u'.' + outExt))
            ++n;
        outPath = dir + u'/' + stem + u'_' + QString::number(n) + u'.' + outExt;
    }
    const QMap<QString, int> delays =
        Ugoira::delaysFromRaw(raw, m_ugoiraDelayMs);
    Ugoira::Frames frames;
    if (!Ugoira::unzipFrames(zipPath, frames, delays, m_ugoiraDelayMs, error))
        return false;
    if (!Ugoira::encodeAnimated(frames, m_ugoiraFormat, outPath,
                                &m_convertCancel, error))
        return false;
    QFile::remove(zipPath);
    const QString md5 = fileContentMd5(outPath);
    if (!md5.isEmpty())
        raw[QStringLiteral("md5")] = md5;
    raw[QStringLiteral("file_ext")] = QFileInfo(outPath).suffix().toLower();
    raw[QStringLiteral("ugoira_converted")] = true;
    raw[QStringLiteral("ugoira_frames")] = frames.images.size();
    const QString newName = QFileInfo(outPath).fileName();
    if (newName != job.fileName)
        emit fileRenamed(job.fileName, newName);
    job.fileName = newName;
    job.filePath = outPath;
    const qsizetype dot = newName.lastIndexOf(u'.');
    job.jsonPath =
        dir + u'/' + (dot > 0 ? newName.left(dot) : newName) + QStringLiteral(".json");
    job.post.rawJson = raw;
    job.post.ext = QFileInfo(outPath).suffix().toLower();
    return true;
}

bool DownloadManager::completeJob(DownloadJob &job, QString *error,
                                   bool *walled) {
    // Last line of defense on every path: a wall page saved as media is
    // worse than a clear failure (it poisons the library silently).
    if (fileLooksLikeHtml(job.filePath)) {
        QFile::remove(job.filePath);
        if (walled)
            *walled = true;
        if (error)
            *error = tr("Server returned a page, not media (challenge "
                        "still active - solve once in Capture in browser…)");
        return false;
    }
    // No API md5 and no md5-looking URL stem (Site_losn123 from
    // imports, Site_456 from id fallback): hash the bytes just
    // written. Boorus index by content md5, so this both names the
    // file canonically and makes md5:<hash> searches work.
    QJsonObject raw = job.post.rawJson;
    const bool hasMd5 =
        !raw.value(QStringLiteral("md5")).toString().isEmpty() ||
        !raw.value(QStringLiteral("checksumMD5")).toString().isEmpty();
    if (!hasMd5) {
        const QString md5 = fileContentMd5(job.filePath);
        if (!md5.isEmpty()) {
            raw[QStringLiteral("md5")] = md5;
            const QString name = RemotePosts::downloadFileName(
                RemotePosts::fsTagForPost(job.post), raw);
            if (name != job.fileName) {
                const QString dir =
                    QFileInfo(job.filePath).absolutePath();
                const QString newPath = dir + u'/' + name;
                const qsizetype dot = name.lastIndexOf(u'.');
                const QString newJson =
                    dir + u'/' + name.left(dot) + QStringLiteral(".json");
                if (!QFile::exists(newPath) &&
                    QFile::rename(job.filePath, newPath)) {
                    emit fileRenamed(job.fileName, name);
                    job.fileName = name;
                    job.filePath = newPath;
                    job.jsonPath = newJson;
                }
            }
            job.post.rawJson = raw; // favourite coupling reuses this
        }
    }
    // Ugoira zips convert to animated webp/jxl in place (when configured):
    // the sidecar below then describes the converted file.
    if (isUgoiraPost(job.post) && m_ugoiraFormat != QStringLiteral("zip")) {
        QString convError;
        if (!convertUgoiraZip(job, raw, &convError)) {
            if (error)
                *error = convError.isEmpty()
                             ? tr("Ugoira conversion failed (zip kept)")
                             : convError;
            return false; // zip stays on disk; message names the cause
        }
        job.post.rawJson = raw;
    }
    // Sidecar: raw API object, content-routed by the scanner on rescan.
    QFile jf(job.jsonPath);
    if (jf.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        jf.write(QJsonDocument(job.post.rawJson)
                     .toJson(QJsonDocument::Indented)); // jq-readable sidecars
        // Every completion path funnels here (network, viewer-cache move,
        // browser backend): cache this post's notes in the sidecar.
        cacheNotesForJob(job);
        return true;
    }
    if (error)
        *error = tr("Cannot write %1").arg(job.jsonPath);
    QFile::remove(job.filePath);
    return false;
}

void DownloadManager::cacheNotesForJob(const DownloadJob &job) {
    const Post &p = job.post;
    // Only posts the API flagged with annotations: this keeps a 100-file
    // batch from firing 100 pointless requests. Older downloads without a
    // cache are backfilled by the viewer on first open.
    if (!p.isRemote || p.id == 0 || !p.hasNotes || job.jsonPath.isEmpty())
        return;
    const BooruSite *site = RemotePosts::entryForPost(p);
    if (!site ||
        PostMeta::notesUrl(site->engine, site->baseUrl, p.id).isEmpty())
        return;
    for (const NotesJob &q : std::as_const(m_notesQueue)) {
        if (q.sidecarPath == job.jsonPath)
            return;
    }
    if (m_notesActiveSidecar == job.jsonPath)
        return;
    m_notesQueue.append({p, job.jsonPath});
    pumpNotesCache();
}

void DownloadManager::pumpNotesCache() {
    if (!m_notesFetcher || !m_notesActiveSidecar.isEmpty())
        return;
    while (!m_notesQueue.isEmpty()) {
        const NotesJob job = m_notesQueue.takeFirst();
        const BooruSite *site = RemotePosts::entryForPost(job.post);
        if (!site ||
            PostMeta::notesUrl(site->engine, site->baseUrl, job.post.id)
                .isEmpty())
            continue;
        if (site->engine == BooruEngine::Danbooru)
            m_notesFetcher->setCreds(site->user, site->secret, {}, {});
        else if (site->engine == BooruEngine::Gelbooru ||
                 site->engine == BooruEngine::Safebooru)
            m_notesFetcher->setCreds({}, {}, site->user, site->secret);
        else
            m_notesFetcher->setCreds({}, {}, {}, {});
        m_notesActiveSidecar = job.sidecarPath;
        m_notesFetcher->fetch(site->key, site->baseUrl, job.post.id,
                              site->engine, true, NotesPart);
        return;
    }
}

void DownloadManager::finishJob(const DownloadJob &job, bool ok,
                                 const QString &error, bool walled, bool repump) {    forgetPersisted(job.post.fileUrl);
    if (!ok && walled) {
        // HTML-where-media-was-expected: remember where, so the solve
        // browser opens AT the wall instead of the homepage.
        if (const BooruSite *e = RemotePosts::entryForPost(job.post))
            m_summary.wallTargets.append({e->key, job.post.fileUrl});
    }
    if (ok) {
        m_summary.done++;
        m_summary.downloaded.append(job.post);
        m_summary.finalPaths.append({job.post.fileUrl, job.filePath});
    } else {
        m_summary.failed++;
        m_summary.errors.append(error.isEmpty() ? job.fileName : error);
        // A failed wall/login/offline attempt must not leave an empty
        // per-site folder behind (folders are made on first bytes).
        removeEmptyJobDir(job.filePath);
    }
    m_done++;
    emit fileFinished(job.fileName, ok, error);
    emit progress(m_done, m_total, ok ? job.fileName : QString());
    if (repump)
        pump();
}

// ── DownloadsDialog ────────────────────────────────────────────

DownloadsDialog::DownloadsDialog(DownloadManager *manager, const QString &destDir,
                                 QWidget *parent)
    : QDialog(parent), m_manager(manager), m_destDir(destDir) {
    setWindowTitle(tr("Downloads"));
    resize(520, 340);
    auto *lay = new QVBoxLayout(this);
    m_bar = new CenterProgressBar(this);
    m_current = new QLabel(this);
    m_current->setStyleSheet(QStringLiteral("color:#4a5068;"));
    m_log = new QListWidget(this);
    lay->addWidget(m_bar);
    lay->addWidget(m_current);
    lay->addWidget(m_log, 1);
    auto *row = new QHBoxLayout();
    auto *openBtn = new QPushButton(tr("Open folder"), this);
    connect(openBtn, &QPushButton::clicked, this,
            [this] { QDesktopServices::openUrl(QUrl::fromLocalFile(m_destDir)); });
    m_cancelBtn = new QPushButton(tr("Cancel"), this);
    connect(m_cancelBtn, &QPushButton::clicked, this, [this] {
        if (!m_manager->isActive())
            return;
        m_manager->cancel();
        m_cancelled = true;
        for (auto it = m_rows.begin(); it != m_rows.end(); ++it) {
            if (it.value() &&
                !it.value()->data(Qt::UserRole + 1).toString().isEmpty()) {
                it.value()->setData(Qt::UserRole + 1, QVariant());
                it.value()->setIcon(UiIcons::icon(
                    QStringLiteral("circle"), UiIcons::normal(), 32));
                it.value()->setText(tr("%1 (cancelled)").arg(it.key()));
            }
        }
        m_current->setText(tr("Cancelled."));
        m_cancelBtn->setEnabled(false);
    });
    m_closeBtn = new QPushButton(tr("Close"), this);
    connect(m_closeBtn, &QPushButton::clicked, this, [this] { accept(); });
    row->addWidget(openBtn);
    auto *browseBtn = new QPushButton(tr("Browse in app"), this);
    browseBtn->setToolTip(tr("Open the download folder as a library tab"));
    connect(browseBtn, &QPushButton::clicked, this,
            [this] { emit browseRequested(m_destDir); });
    row->addWidget(browseBtn);
    row->addStretch(1);
    row->addWidget(m_cancelBtn);
    row->addWidget(m_closeBtn);
    lay->addLayout(row);
}

void DownloadsDialog::setTotal(int n) {
    m_bar->setRange(0, qMax(1, n));
    m_bar->setValue(0);
}

void DownloadsDialog::onBatchStarted(int total) {
    // New batch: start the log over (the dialog outlives batches).
    m_log->clear();
    m_rows.clear();
    m_cancelled = false;
    m_cancelBtn->setEnabled(true);
    m_cancelBtn->setText(tr("Cancel"));
    m_bar->setRange(0, qMax(1, total));
    m_bar->setValue(0);
}

void DownloadsDialog::onProgress(int done, int total, const QString &current) {
    m_bar->setRange(0, qMax(1, total));
    m_bar->setValue(done);
    if (!current.isEmpty())
        m_current->setText(current);
}

void DownloadsDialog::addRow(const QString &fileName, const QString &text,
                            const QIcon &icon) {
    QListWidgetItem *item = m_rows.value(fileName, nullptr);
    if (item) {
        item->setText(text);
        item->setIcon(icon);
    } else {
        item = new QListWidgetItem(icon, text, m_log);
        m_rows.insert(fileName, item);
    }
    m_log->scrollToBottom();
}

void DownloadsDialog::onFileStarted(const QString &fileName) {
    if (fileName.isEmpty())
        return;
    addRow(fileName, fileName,
           UiIcons::icon(QStringLiteral("download"), UiIcons::accent(), 32));
    if (QListWidgetItem *item = m_rows.value(fileName, nullptr))
        item->setData(Qt::UserRole + 1, QStringLiteral("active"));
    m_current->setText(tr("Downloading %1…").arg(fileName));
}

void DownloadsDialog::onFileFinished(const QString &fileName, bool ok,
                                     const QString &error) {
    if (fileName.isEmpty())
        return;
    if (ok) {
        addRow(fileName, fileName,
               UiIcons::icon(QStringLiteral("check"), QColor(0x43, 0xd1, 0x7c),
                             32));
        if (QListWidgetItem *item = m_rows.value(fileName, nullptr))
            item->setData(Qt::UserRole + 1, QVariant());
        m_current->setText(tr("Saved %1").arg(fileName));
    } else {
        const QString why = error.isEmpty() ? fileName : error;
        addRow(fileName, tr("%1 - %2").arg(fileName, why),
               UiIcons::icon(QStringLiteral("x"), QColor(0xe0, 0x55, 0x61), 32));
        if (QListWidgetItem *item = m_rows.value(fileName, nullptr)) {
            item->setData(Qt::UserRole, why);
            item->setData(Qt::UserRole + 1, QVariant());
            item->setToolTip(why);
            item->setForeground(QColor(0xe0, 0x55, 0x55));
        }
        m_current->setText(tr("Failed %1").arg(fileName));
    }
}

void DownloadsDialog::onFileSkipped(const QString &fileName) {
    if (fileName.isEmpty())
        return;
    addRow(fileName, tr("%1 (already on disk)").arg(fileName),
           UiIcons::icon(QStringLiteral("minus"), UiIcons::normal(), 32));
}

void DownloadsDialog::onFileRenamed(const QString &oldName, const QString &newName) {
    auto it = m_rows.find(oldName);
    if (it == m_rows.end() || !it.value())
        return;
    QListWidgetItem *item = it.value();
    m_rows.erase(it);
    item->setText(newName);
    m_rows.insert(newName, item);
    if (m_current->text().contains(oldName))
        m_current->setText(m_current->text().replace(oldName, newName));
}

void DownloadsDialog::onFinished(const DownloadSummary &summary) {
    m_bar->setValue(m_bar->maximum());
    m_current->setText(tr("Done: %1  Skipped: %2  Failed: %3")
                           .arg(summary.done)
                           .arg(summary.skipped)
                           .arg(summary.failed));
    if (summary.failed == 0 && !m_cancelled)
        m_log->addItem(tr("All files downloaded."));
    m_log->scrollToBottom();
    m_cancelBtn->setEnabled(false);
}
