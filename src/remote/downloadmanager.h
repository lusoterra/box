#pragma once

// Batch downloader for remote posts. Reimplements gallery-dl's core loop at
// minimum scope: fetch file_url, store next to a sidecar carrying the raw
// API object. Sidecars reuse this app's own layouts so downloads are
// instantly browsable:
//
//   <Site>_<md5>.<ext>  +  <Site>_<md5>.json   (native API object)
//
// The scanner content-routes native Danbooru/Gelbooru/Moebooru objects, so
// no conversion step is needed - tags are stored exactly as the API gave
// them, like gallery-dl --write-info-json would.
#include <QDialog>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QObject>
#include <QString>
#include <QVector>

#include "post.h"

class QNetworkAccessManager;
class WebDownloadBackend; // webdlbackend.h (injected by the app)

struct DownloadJob {
    Post post;
    QString fileName; // <Site>_<md5>.<ext>
    QString filePath; // dest/<Site>/<fileName>
    QString jsonPath; // dest/<Site>/<stem>.json
};

struct DownloadSummary {
    int done = 0;
    int skipped = 0; // already on disk
    int failed = 0;
    QStringList errors;
    QVector<Post> downloaded; // successfully saved posts (for auto-favourite)
    // Final on-disk paths by file URL (after any md5 rename below).
    QList<QPair<QString, QString>> finalPaths;
};

class DownloadManager : public QObject {
    Q_OBJECT
public:
    explicit DownloadManager(QObject *parent = nullptr);

    // Build target paths for posts under destDir (one subfolder per site).
    static QList<DownloadJob> plan(const QVector<Post> &posts, const QString &destDir);

    void enqueue(const QList<DownloadJob> &jobs);
    void cancel();
    bool isActive() const { return m_active; }
    // Browser-backed media for fingerprint-walled hosts (null = QNAM
    // for everything). Set once by the app.
    void setWebBackend(WebDownloadBackend *backend);

signals:
    void progress(int done, int total, const QString &current);
    // Live per-file bytes (drives the import dialog's per-file rows).
    void fileProgress(const QString &filePath, qint64 received, qint64 total);
    // Live per-file lifecycle for the downloads popup log (also fired
    // for batches the popup didn't open: it shows them on demand).
    void fileStarted(const QString &fileName);
    void fileFinished(const QString &fileName, bool ok, const QString &error);
    void fileSkipped(const QString &fileName); // already on disk
    // A finished file landed under a different name than planned (md5
    // rename below): retitle its in-flight log row.
    void fileRenamed(const QString &oldName, const QString &newName);
    void finished(const DownloadSummary &summary);

private slots:
    void onWebProgress(qint64 id, qint64 received, qint64 total);
    void onWebFinished(qint64 id, bool ok, const QString &error);

private:
    struct Flight {
        DownloadJob job;
        class QNetworkReply *reply = nullptr;
        class QFile *file = nullptr;
        qint64 webId = -1; // >= 0 while a browser-backend flight runs
    };
    void pump();
    void onFileReply();
    void finishJob(const DownloadJob &job, bool ok, const QString &error);
    // Post-write finalization shared by both backends: content sniff,
    // md5 hash-and-rename for nameless files, sidecar write.
    // Returns false with error set when the file must be dropped.
    bool completeJob(DownloadJob &job, QString *error);
    Flight *findWebFlight(qint64 webId);
    QNetworkAccessManager *namForProxy(const QString &proxySpec);

    class QNetworkAccessManager *m_nam;
    QList<DownloadJob> m_queue;
    QList<Flight *> m_flights;
    WebDownloadBackend *m_webBackend = nullptr;
    qint64 m_webNextId = 1;
    QMap<QString, class QNetworkAccessManager *> m_proxyNams;
    DownloadSummary m_summary;
    int m_done = 0;
    int m_total = 0;
    bool m_active = false;
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
    void addRow(const QString &fileName, const QString &text);

private:
    DownloadManager *m_manager;
    QString m_destDir;
    class QProgressBar *m_bar;
    class QLabel *m_current;
    class QListWidget *m_log;
    class QPushButton *m_closeBtn;
    bool m_cancelled = false;
    // In-flight rows by file name, updated in place as each file lands.
    QMap<QString, class QListWidgetItem *> m_rows;
};
