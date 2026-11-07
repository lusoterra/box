#pragma once

// LoliSnatcher (LOSN) backup import: reads a store.db (BooruItem + Tag +
// ImageTag tables) and brings entries over as favourites and/or downloads.
//
//   isFavourite=1            -> favourite entry (DB only, no files)
//   isSnatched=1 + download  -> full-quality file + gallery-dl-style sidecar
//   isSnatched=1 + fav-only  -> favourite entry instead of files
//
// Downloaded sidecars are gallery-dl native JSON, never LOSN JSON: when the
// source API answers, the REAL post object is stored (re-fetched by post
// URL); otherwise a native-shaped object is synthesized from the backup
// row. Either way the scanner reads it back seamlessly.
#include <QDialog>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVector>

#include "settings.h" // AppSettings member by value
#include "downloadmanager.h" // DownloadSummary slot arg

struct LosnRow {
    qint64 rowId = 0;
    QString thumbUrl, sampleUrl, fileUrl, postUrl, mediaType;
    bool snatched = false;
    bool favourite = false;
    QStringList tags;
    QString siteKey;   // danbooru|gelbooru|... or "" when unknown
    QString siteLabel; // "Gelbooru", "Rule34", ...
    qint64 postId = 0; // from the post URL, 0 when unparseable
    QString md5;       // 32-hex stem from the file URL, "" when absent
};

// Pure row parsing (unit-tested): host -> site, URL -> post id / md5.
namespace LosnImport {
QList<LosnRow> readBackup(const QString &dbPath, QString &error);
void siteForHost(const QString &host, QString &key, QString &label);
qint64 idFromPostUrl(const QString &postUrl);
QString md5FromFileUrl(const QString &fileUrl);
// md5 stems already on disk under destDir (any site subfolder):
// re-imports skip these without re-downloading, even when an earlier
// run filed them under a different label (merge_* orphans).
QSet<QString> md5sOnDisk(const QString &destDir);
// Gallery-dl native-shaped fallback sidecar built from a backup row.
QJsonObject nativeSidecar(const LosnRow &row);
// Download target paths for a row (<Site>_<md5>.<ext> + sidecar).
// Pure: re-imports diff against these before any network use.
struct DownloadPaths {
    QString fileName;
    QString filePath;
    QString jsonPath;
};
DownloadPaths targetPaths(const LosnRow &row, const QString &destDir);
// Naming that prefers a fetched sidecar's md5: targetPaths only knows
// the URL-guessed md5 (empty for losn rows), so refreshed rows would
// otherwise keep Site_losn123 names despite carrying a real md5.
DownloadPaths pathsForRow(const LosnRow &row, const QJsonObject &sidecar,
                          const QString &destDir);
// Summary diff for the import dialog: split the manager's final on-disk
// paths (fileURL -> path) into new files (md5 not in beforeMd5) vs
// already-present (md5 was in beforeMd5). Names only, for the log.
struct SummaryDiff {
    QStringList newFiles;
    QStringList alreadyPresent;
};
SummaryDiff diffNewFiles(const QSet<QString> &beforeMd5,
                         const QList<QPair<QString, QString>> &finalPaths);
} // namespace LosnImport

class FavDb;
class DownloadManager;
struct AppSettings;

class LosnImportDialog : public QDialog {
    Q_OBJECT
signals:
    void wallSolveRequested(const QString &siteKey, const QUrl &startUrl);
public:
    LosnImportDialog(const QString &dbPath, FavDb *favDb, DownloadManager *dl,
                     const AppSettings &settings, const QString &downloadDir,
                      QWidget *parent = nullptr);

private slots:
    void onStart();
    void pumpMeta(); // keep up to kMetaFlights metadata fetches in flight
    void startDownloads();
    void onMetaDone(qint64 reqId, const QJsonObject &raw);
    void onMetaFailed(qint64 reqId, const QString &error);
    void onFileProgress(const QString &filePath, qint64 received, qint64 total);
    void onDownloadsDone(const DownloadSummary &summary);

private:
    void log(const QString &text);
    void setBusy(bool busy);

    QString m_dbPath;
    QString m_downloadDir;
    FavDb *m_favDb;
    DownloadManager *m_downloads;
    AppSettings m_settings;
    QList<LosnRow> m_rows;
    QHash<QString, int> m_rowIndex; // fileUrl -> position in m_rows
    int m_favCount = 0;
    int m_snatchCount = 0;
    // options
    class QRadioButton *m_snatchedDownload = nullptr;
    class QRadioButton *m_snatchedFav = nullptr;
    class QRadioButton *m_snatchedSkip = nullptr;
    class QCheckBox *m_importFavs = nullptr;
    class QCheckBox *m_favDownloaded = nullptr;
    class QCheckBox *m_downloadFavs = nullptr;
    class QCheckBox *m_sync = nullptr;
    class QLabel *m_counts = nullptr;
    class QLabel *m_target = nullptr;
    class QProgressBar *m_bar = nullptr;
    class QListWidget *m_log = nullptr;
    class QPushButton *m_startBtn = nullptr;
    // run state
    QList<LosnRow> m_downloadRows; // union selected to download this run
    QList<LosnRow> m_metaPending; // rows still needing a metadata fetch
    // On-disk rows: media stays, but the sidecar is re-fetched and
    // rewritten (a fallback sidecar from an earlier run is never kept
    // when the API answers).
    QList<LosnRow> m_refreshPending;
    QSet<QString> m_refreshUrls; // fileUrl -> sidecar-refresh purpose
    int m_refreshDone = 0;
    int m_refreshTotal = 0;
    int m_refreshOk = 0;
    int m_refreshFailed = 0;
    QHash<QString, QJsonObject> m_sidecars; // fileUrl -> sidecar object
    int m_metaDone = 0;
    int m_metaTotal = 0;
    int m_metaFailed = 0;
    qint64 m_nextReqId = 1;
    bool m_expectingDownloads = false;
    // Parallel metadata flights (huge backups): reqId -> fileUrl.
    static constexpr int kMetaFlights = 4;
    QVector<class PostRefresher *> m_metaFlights;
    QHash<qint64, QString> m_flightUrl; // reqId -> fileUrl
    QHash<qint64, class PostRefresher *> m_reqFlight; // reqId -> busy flight
    // Live per-file download rows (path -> item) + last shown percent.
    QHash<QString, class QListWidgetItem *> m_fileRows;
    QHash<QString, int> m_filePct;
    QHash<QString, QString> m_jobPaths; // planned file name -> file path
    // Pre-import disk snapshot (md5 stems) for the new-file summary diff.
    QSet<QString> m_beforeMd5;
    void onFileRenamed(const QString &oldName, const QString &newName);
};
