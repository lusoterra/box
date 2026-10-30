#include "downloadmanager.h"
#include "imagecache.h"
#include "filehash.h"
#include "progressbar.h"
#include "remotesource.h"
#include "ugoioconvert.h"
#include "webdlbackend.h"

#include <QDesktopServices>
#include <QDialogButtonBox>
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
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QSet>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>

DownloadManager::DownloadManager(QObject *parent)
    : QObject(parent), m_nam(new QNetworkAccessManager(this)) {}

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
// Site folder segment: hostname for configured sites (see
// RemotePosts::fsTagForPost), legacy label otherwise; settings.ini is
// hand-editable either way, so a hostile "../.." tag must never become
// a path. Separators and colons (IPv6/port leftovers) are flattened
// and bare dot-names prefixed; everything else (dots, spaces, unicode,
// "_Stray") passes through untouched so existing folders keep working.
QString safeSiteDir(const QString &tag) {
    QString seg = tag;
    seg.replace(u'/', u'_').replace(u'\\', u'_').replace(u':', u'_');
    while (seg == QStringLiteral(".") || seg == QStringLiteral(".."))
        seg.prepend(u'_');
    if (seg.isEmpty())
        seg = QStringLiteral("Booru");
    return seg;
}
// 32-hex file stems double as content hashes on most boorus (Gelbooru,
// Safebooru, Danbooru CDN...). Same rule as LosnImport::md5FromFileUrl.
QString md5Stem(const QString &fileUrl) {
    const QString stem = QUrl(fileUrl).fileName().split(u'.').first();
    static const QRegularExpression hex32(QStringLiteral("^[0-9a-f]{32}$"));
    return hex32.match(stem).hasMatch() ? stem : QString();
}
} // namespace

void DownloadManager::setImageCache(ImageCache *cache) {
    m_imgCache = cache;
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
        const QString tag = RemotePosts::fsTagForPost(p);
        const QString dir = destDir + u'/' + safeSiteDir(tag);
        QString name = RemotePosts::downloadFileName(tag, raw);
        if (used.contains(dir + u'/' + name)) {
            const int dot = name.lastIndexOf(u'.');
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
        const int dot = name.lastIndexOf(u'.');
        job.jsonPath = dir + u'/' + name.left(dot) + QStringLiteral(".json");
        used.insert(job.filePath);
        jobs.append(job);
    }
    return jobs;
}

void DownloadManager::enqueue(const QList<DownloadJob> &jobs) {
    cancel();
    QList<DownloadJob> skipped;
    for (const DownloadJob &j : jobs) {
        if (QFile::exists(j.filePath)) {
            m_summary.skipped++;
            // Already on disk still counts as downloaded for the
            // favourite coupling (otherwise re-downloads never get
            // favourited/linked and Downloads/Favourites diverge).
            m_summary.downloaded.append(j.post);
            m_summary.finalPaths.append({j.post.fileUrl, j.filePath});
            skipped.append(j);
            continue;
        }
        m_queue.append(j);
    }
    m_done = 0;
    m_total = m_queue.size() + m_summary.skipped;
    if (m_queue.isEmpty()) {
        emit finished(m_summary);
        return;
    }
    m_convertCancel.storeRelaxed(0); // fresh batch clears any abort
    m_active = true;
    emit progress(0, m_total, QString());
    for (const DownloadJob &j : skipped)
        emit fileSkipped(j.fileName);
    pump();
}

void DownloadManager::cancel() {
    m_convertCancel.storeRelaxed(1); // abort any in-flight ugoira encode
    for (Flight *f : m_flights) {
        if (f->webId >= 0 && m_webBackend)
            m_webBackend->cancel(f->webId); // backend drops partial files
        if (f->reply) {
            disconnect(f->reply, nullptr, this, nullptr);
            f->reply->abort();
            f->reply->deleteLater();
        }
        if (f->file) {
            f->file->close();
            f->file->remove(); // partial file
            delete f->file;
        }
        delete f;
    }
    m_flights.clear();
    m_queue.clear();
    m_active = false;
    m_summary = DownloadSummary();
}

void DownloadManager::cancelUrls(const QSet<QString> &fileUrls) {
    if (fileUrls.isEmpty())
        return;
    const int before = m_queue.size() + m_flights.size();
    m_queue.erase(std::remove_if(m_queue.begin(), m_queue.end(),
                                 [&](const DownloadJob &j) {
                                     return fileUrls.contains(j.post.fileUrl);
                                 }),
                  m_queue.end());
    QList<Flight *> dead;
    for (Flight *f : m_flights) {
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
            f->file->remove(); // partial file
            delete f->file;
        }
        emit fileFinished(f->job.fileName, false, tr("Cancelled"));
        delete f;
    }
    m_total = qMax(0, m_total - (before - m_queue.size() - m_flights.size()));
    if (m_queue.isEmpty() && m_flights.isEmpty())
        m_active = false;
    else
        pump(); // fill freed flight slots
}

void DownloadManager::pump() {
    while (m_active && m_flights.size() < kMaxFlight && !m_queue.isEmpty()) {
        DownloadJob job = m_queue.takeFirst();
        QDir().mkpath(QFileInfo(job.filePath).absolutePath());
        if (m_imgCache && !job.post.fileUrl.isEmpty()) {
            // Already on disk at full resolution (viewed): copy the
            // bytes instead of re-fetching. Cache holds complete
            // image/video bodies only (walls never land there).
            const QString have = m_imgCache->localPath(job.post.fileUrl);
            if (!have.isEmpty() && QFile::copy(have, job.filePath)) {
                QString error;
                const bool ok = completeJob(job, &error);
                emit fileStarted(job.fileName);
                emit progress(m_done, m_total, job.fileName);
                // No repump: the outer loop keeps filling flights, so a
                // long all-cached run stays flat instead of recursing.
                finishJob(job, ok, error, false, false);
                continue;
            }
        }
        if (m_webBackend && RemotePosts::needsWebDownload(job.post)) {
            // Fingerprint-walled media: the browser stack downloads
            // with the profile's session (plain requests get wall
            // pages instead of bytes).
            auto *flight = new Flight{job, nullptr, nullptr};
            flight->webId = m_webNextId++;
            m_flights.append(flight);
            emit fileStarted(job.fileName);
            emit progress(m_done, m_total, job.fileName);
            m_webBackend->start(flight->webId, job.post.fileUrl,
                                job.filePath);
            continue;
        }
        auto *file = new QFile(job.filePath);
        if (!file->open(QIODevice::WriteOnly)) {
            delete file;
            const QString name = job.fileName;
            finishJob(job, false, tr("Cannot write %1").arg(job.filePath));
            continue;
        }
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
        auto *flight = new Flight{job, nullptr, file};
        flight->reply = namForProxy(proxySpecForJob(job))->get(req);
        connect(flight->reply, &QNetworkReply::finished, this,
                &DownloadManager::onFileReply);
        connect(flight->reply, &QIODevice::readyRead, this, [this, flight] {
            if (flight->file)
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
    if (m_active && m_flights.isEmpty()) {
        m_active = false;
        emit finished(m_summary);
    }
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
    if (ok && flight->file) {
        const QString ct = reply
                               ->header(QNetworkRequest::ContentTypeHeader)
                               .toString()
                               .trimmed()
                               .toLower();
        // Accept image/video/octet-stream content types. Some servers emit
        // whitespace or odd casing around the MIME type; trimmed() + case
        // handling below keeps wall pages out while letting real media
        // (e.g. video/webm) through.
        const bool isMedia = ct.startsWith(QStringLiteral("image/"), Qt::CaseInsensitive) ||
                             ct.startsWith(QStringLiteral("video/"), Qt::CaseInsensitive) ||
                             ct.startsWith(QStringLiteral("application/octet-stream"), Qt::CaseInsensitive) ||
                             ct.startsWith(QStringLiteral("binary/octet-stream"), Qt::CaseInsensitive);
        if (!ct.isEmpty() && !isMedia) {
            // Wall/login page instead of bytes: never keep it as media.
            // Sniff the head off disk (the body already streamed into
            // the file) so the error names a captcha/wall and points
            // at cookie capture instead of quoting a content type.
            ok = false;
            flight->file->write(reply->readAll());
            flight->file->close();
            flight->file = nullptr; // closed+kept: cleanup below only deletes
            QByteArray head;
            QFile probe(job.filePath);
            if (probe.open(QIODevice::ReadOnly))
                head = probe.read(65536);
            const bool isWall =
                RemotePosts::looksLikeChallengePage(head) ||
                RemotePosts::looksLikeCloudflareWall(head);
            error = RemotePosts::wallAwareMediaError(
                QUrl(job.post.fileUrl).host(), head,
                reply->header(QNetworkRequest::ContentTypeHeader).toString());
            if (isWall) {
                QFile::remove(job.filePath);
                reply->deleteLater();
                finishJob(job, false, error, true);
                delete flight;
                return;
            }
        } else {
            flight->file->write(reply->readAll()); // trailing bytes
            flight->file->close();
            bool walled = false;
            if (!completeJob(job, &error, &walled))
                ok = false;
            if (walled) {
                reply->deleteLater();
                finishJob(job, ok, error, true);
                delete flight;
                return;
            }
        }
    } else if (!ok) {
        const int http =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        error = http > 0 ? tr("HTTP %1").arg(http)
                         : reply->errorString();
    }
    if (flight->file) {
        if (!ok) {
            flight->file->close();
            flight->file->remove();
        }
        delete flight->file;
    }
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
    delete flight;
    QString error = errorIn;
    // The backend drops partial files itself on failure.
    bool walled = false;
    if (ok && !completeJob(job, &error, &walled))
        ok = false;
    finishJob(job, ok, error, walled);
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
    const int dot = newName.lastIndexOf(u'.');
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
                const int dot = name.lastIndexOf(u'.');
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
        return true;
    }
    if (error)
        *error = tr("Cannot write %1").arg(job.jsonPath);
    QFile::remove(job.filePath);
    return false;
}

void DownloadManager::finishJob(const DownloadJob &job, bool ok,
                                 const QString &error, bool walled, bool repump) {
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
    m_closeBtn = new QPushButton(tr("Cancel"), this);
    connect(m_closeBtn, &QPushButton::clicked, this, [this] {
        if (m_manager->isActive()) {
            m_manager->cancel();
            m_cancelled = true;
            // In-flight rows never get their finished event: mark them.
            for (auto it = m_rows.begin(); it != m_rows.end(); ++it) {
                if (it.value() && it.value()->text().startsWith(QStringLiteral("↓")))
                    it.value()->setText(tr("○ %1 (cancelled)").arg(it.key()));
            }
            m_current->setText(tr("Cancelled."));
            m_closeBtn->setText(tr("Close"));
        } else {
            accept();
        }
    });
    row->addWidget(openBtn);
    auto *browseBtn = new QPushButton(tr("Browse in app"), this);
    browseBtn->setToolTip(tr("Open the download folder as a library tab"));
    connect(browseBtn, &QPushButton::clicked, this,
            [this] { emit browseRequested(m_destDir); });
    row->addWidget(browseBtn);
    row->addStretch(1);
    row->addWidget(m_closeBtn);
    lay->addLayout(row);
}

void DownloadsDialog::setTotal(int n) {
    m_bar->setRange(0, qMax(1, n));
    m_bar->setValue(0);
}

void DownloadsDialog::onProgress(int done, int total, const QString &current) {
    if (done == 0) {
        // New batch: start the log over (the dialog outlives batches).
        m_log->clear();
        m_rows.clear();
        m_cancelled = false;
        m_closeBtn->setText(tr("Cancel"));
    }
    m_bar->setRange(0, qMax(1, total));
    m_bar->setValue(done);
    if (!current.isEmpty())
        m_current->setText(current);
}

void DownloadsDialog::addRow(const QString &fileName, const QString &text) {
    QListWidgetItem *item = m_rows.value(fileName, nullptr);
    if (item) {
        item->setText(text);
    } else {
        item = new QListWidgetItem(text, m_log);
        m_rows.insert(fileName, item);
    }
    m_log->scrollToBottom();
}

void DownloadsDialog::onFileStarted(const QString &fileName) {
    if (fileName.isEmpty())
        return;
    addRow(fileName, tr("↓ %1").arg(fileName));
    m_current->setText(tr("Downloading %1…").arg(fileName));
}

void DownloadsDialog::onFileFinished(const QString &fileName, bool ok,
                                     const QString &error) {
    if (fileName.isEmpty())
        return;
    if (ok) {
        addRow(fileName, tr("✓ %1").arg(fileName));
        m_current->setText(tr("Saved %1").arg(fileName));
    } else {
        const QString why = error.isEmpty() ? fileName : error;
        addRow(fileName, tr("✗ %1 - %2").arg(fileName, why));
        m_current->setText(tr("Failed %1").arg(fileName));
    }
}

void DownloadsDialog::onFileSkipped(const QString &fileName) {
    if (fileName.isEmpty())
        return;
    addRow(fileName, tr("- %1 (already on disk)").arg(fileName));
}

void DownloadsDialog::onFileRenamed(const QString &oldName, const QString &newName) {
    auto it = m_rows.find(oldName);
    if (it == m_rows.end() || !it.value())
        return;
    QListWidgetItem *item = it.value();
    m_rows.erase(it);
    item->setText(tr("↓ %1").arg(newName));
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
    m_closeBtn->setText(tr("Close"));
}
