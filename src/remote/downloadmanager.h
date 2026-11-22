#pragma once

// Batch downloader for remote posts. Reimplements gallery-dl's core loop at
// minimum scope: fetch file_url, store next to a sidecar carrying the raw
// API object. Sidecars reuse this app's own layouts so downloads are
// instantly browsable:
//
//   <host>_<md5>.<ext>  +  <host>_<md5>.json   (native API object)
//   (host = the entry's base-URL host; legacy <Label>_* files still read)
//
// The scanner content-routes native Danbooru/Gelbooru/Moebooru objects, so
// no conversion step is needed - tags are stored exactly as the API gave
// them, like gallery-dl --write-info-json would.
#include <QDialog>
#include <QAtomicInt>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QObject>
#include <QSet>
#include <QString>
#include <QVector>

#include "post.h"

class QNetworkAccessManager;
class QIcon;
class WebDownloadBackend; // webdlbackend.h (injected by the app)
class DownloadQueueStore; // downloadqueue.h

struct DownloadJob {
    Post post;
    QString fileName; // <host>_<md5>.<ext>
    QString filePath; // dest/<host>/<fileName>
    QString jsonPath; // dest/<host>/<stem>.json
    // QNAM answered a challenge/login page, so this job was re-run through
    // the capture browser. One retry only: a browser that also walls ends
    // as a normal error instead of looping.
    bool webTried = false;
};

struct DownloadSummary {
    int done = 0;
    int skipped = 0; // already on disk
    int failed = 0;
    QStringList errors;
    // (siteKey, walled file URL) for HTML-where-media-was-expected:
    // the solve browser opens AT the wall instead of the homepage.
    QList<QPair<QString, QString>> wallTargets;
    QVector<Post> downloaded; // saved or already-on-disk posts (for auto-favourite)
    // Final on-disk paths by file URL (after any md5 rename below).
    QList<QPair<QString, QString>> finalPaths;
};

class DownloadManager : public QObject {
    Q_OBJECT
public:
    explicit DownloadManager(QObject *parent = nullptr);
    ~DownloadManager() override;

    // Build target paths for posts under destDir (one subfolder per site).
    static QList<DownloadJob> plan(const QVector<Post> &posts, const QString &destDir);
    // Drop later jobs that target the same file path as an earlier one
    // (two backup rows can share a content hash/path). Skipped jobs are
    // appended to `skipped` when given. Pure.
    static QList<DownloadJob> dedupeJobs(const QList<DownloadJob> &jobs,
                                         QList<DownloadJob> *skipped = nullptr);

    // Sidecar candidate paths for a media file (stem form + flat form,
    // deduped): the downloads write the stem form (Site_md5.json),
    // gallery-dl writes the flat form (Site_md5.jpg.json). Pure.
    static QStringList sidecarPathsFor(const QString &absFile);

    void enqueue(const QList<DownloadJob> &jobs);
    void cancel();
    // Stop accepting new work but let flights already in progress finish
    // (quit path: "finish current, then quit"). Queued jobs stay in the
    // journal so the next launch can resume them.
    void stopAfterCurrent();
    // Close-time teardown: abort sockets and drop partial files, but keep
    // the persisted queue for the next launch (unlike cancel(), which is
    // an explicit user abort and forgets the batch).
    void shutdown();
    // Crash-safe queue journal. Empty path disables persistence.
    void setQueueStorePath(const QString &path);
    QList<DownloadJob> persistedJobs() const;
    void resumePersistedQueue();
    void discardPersistedQueue(); // forget rows + stray .part files
    int pendingCount() const; // queued + in-flight + waiting on the cache
    // Drop queued/in-flight jobs for these file URLs (explicit unfavourite:
    // a finishing download must neither resurrect the row nor rewrite the
    // deleted file). Dropped flights report Cancelled, never failure.
    void cancelUrls(const QSet<QString> &fileUrls);
    bool isActive() const { return m_active; }
    // Browser-backed media for fingerprint-walled hosts (null = QNAM
    // for everything). Set once by the app.
    void setWebBackend(WebDownloadBackend *backend);
    void setImageCache(class ImageCache *cache); // full-res shortcut source
    // Ugoira handling: "webp" | "jxl" converts served .zips to animated
    // files on completion (ffmpeg required, +cjxl for jxl); anything else
    // keeps the zip. Converted files replace the zip.
    void setUgoiraFormat(const QString &format) { m_ugoiraFormat = format; }
    void setUgoiraFrameDelayMs(int ms) { m_ugoiraDelayMs = qBound(20, ms, 2000); }

signals:
    void progress(int done, int total, const QString &current);
    // Live per-file bytes (drives the import dialog's per-file rows).
    void fileProgress(const QString &filePath, qint64 received, qint64 total);
    // Live per-file lifecycle for the downloads popup log (also fired
    // for batches the popup didn't open: it shows them on demand).
    void fileStarted(const QString &fileName);
    void fileFinished(const QString &fileName, bool ok, const QString &error);
    void fileSkipped(const QString &fileName); // already on disk
    // A fresh batch replaced the previous one (progress restarts at 0/N).
    // Appended batches do not emit this, so the dialog keeps its log.
    void batchStarted(int total);
    // Explicit Cancel (downloads dialog) ended a running batch: any
    // pending "quit when downloads finish" decision is void.
    void cancelled();
    // A finished file landed under a different name than planned (md5
    // rename below): retitle its in-flight log row.
    void fileRenamed(const QString &oldName, const QString &newName);
    void finished(const DownloadSummary &summary);

private slots:
    void onWebProgress(qint64 id, qint64 received, qint64 total);
    void onWebFinished(qint64 id, bool ok, const QString &error);
    // The viewer's full-res fetch finished (or failed): resume jobs that
    // were parked waiting for its bytes instead of fetching them twice.
    void onCacheReady(const QString &url, const QString &localPath);
    void onCacheFailed(const QString &url, const QString &error);

private:
    struct Flight {
        DownloadJob job;
        class QNetworkReply *reply = nullptr;
        class QFile *file = nullptr;
        qint64 webId = -1; // >= 0 while a browser-backend flight runs
        QString partPath;  // bytes land here, renamed to job.filePath on success
        bool writeError = false; // staging .part could not be created
    };
    void pump();
    void startWebFlight(const DownloadJob &job);
    // Create the per-site folder + `.part` lazily, on the first bytes.
    bool openStagingFile(Flight *flight);
    // Folders are created on first bytes, never at plan/enqueue time. If a
    // failed job leaves one empty (wall/login page, offline), drop it again
    // unless another pending job needs the same folder.
    void removeEmptyDir(const QString &dirPath);
    void removeEmptyJobDir(const QString &filePath);
    bool dirHasPendingJob(const QString &dirPath) const;
    void onFileReply();
    void finishJob(const DownloadJob &job, bool ok, const QString &error,
                   bool walled = false, bool repump = true);
    // Post-write finalization shared by both backends: content sniff,
    // md5 hash-and-rename for nameless files, sidecar write.
    // Returns false with error set when the file must be dropped.
    bool completeJob(DownloadJob &job, QString *error,
                     bool *walled = nullptr);
    Flight *findWebFlight(qint64 webId);
    QNetworkAccessManager *namForProxy(const QString &proxySpec);
    void persistAccepted(const QList<DownloadJob> &jobs);
    void forgetPersisted(const QString &url);

    class QNetworkAccessManager *m_nam;
    QList<DownloadJob> m_queue;
    QList<Flight *> m_flights;
    // Jobs whose bytes are currently being fetched into the viewer cache.
    // Parked here (and the cache fetch pinned) instead of starting a second
    // request; navigation must not abort them.
    QList<DownloadJob> m_cacheWait;
    QSet<QString> m_cachePinned;
    DownloadQueueStore *m_store = nullptr;
    WebDownloadBackend *m_webBackend = nullptr;
    class ImageCache *m_imgCache = nullptr; // viewer-warmed full bytes
    qint64 m_webNextId = 1;
    QMap<QString, class QNetworkAccessManager *> m_proxyNams;
    DownloadSummary m_summary;
    int m_done = 0;
    int m_total = 0;
    bool m_active = false;
    // Sidecar note caching (download-time): one PostMetaFetcher, jobs
    // queued per finished post so the notes land in `box_notes`.
    struct NotesJob {
        Post post;
        QString sidecarPath;
    };
    QList<NotesJob> m_notesQueue;
    class PostMetaFetcher *m_notesFetcher = nullptr;
    QString m_notesActiveSidecar;
    void cacheNotesForJob(const DownloadJob &job);
    void pumpNotesCache();
    QString m_ugoiraFormat = QStringLiteral("webp");
    int m_ugoiraDelayMs = 100;
    QAtomicInt m_convertCancel{0}; // set by cancel(), polled by converts
    bool convertUgoiraZip(DownloadJob &job, QJsonObject &raw, QString *error);
    static constexpr int kMaxFlight = 3;
};

// Simple progress dialog: overall bar, current file, log, cancel/close.
class DownloadsDialog : public QDialog {
    Q_OBJECT
public:
    explicit DownloadsDialog(DownloadManager *manager, const QString &destDir,
                             QWidget *parent = nullptr);
    void setTotal(int n);

public slots:
    void onBatchStarted(int total);
    void onProgress(int done, int total, const QString &current);
    void onFileStarted(const QString &fileName);
    void onFileFinished(const QString &fileName, bool ok, const QString &error);
    void onFileSkipped(const QString &fileName);
    void onFileRenamed(const QString &oldName, const QString &newName);
    void onFinished(const DownloadSummary &summary);

signals:
    void browseRequested(const QString &dir);

private:
    // Append a log row for a file, or refresh its in-flight row in place.
    // The row icon mirrors the state with Lucide art (no font glyphs).
    void addRow(const QString &fileName, const QString &text,
                const QIcon &icon = {});

private:
    DownloadManager *m_manager;
    QString m_destDir;
    class QProgressBar *m_bar;
    class QLabel *m_current;
    class QListWidget *m_log;
    class QPushButton *m_cancelBtn;
    class QPushButton *m_closeBtn;
    bool m_cancelled = false;
    // In-flight rows by file name, updated in place as each file lands.
    QMap<QString, class QListWidgetItem *> m_rows;
};
