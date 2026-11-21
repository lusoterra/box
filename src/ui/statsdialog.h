#pragma once

// Library statistics popup (Info -> Statistics…).
//
// Numbers come from two places: the favourites DB is read on the UI
// thread (SQLite connection safety), while the download folder and the
// cache trees are walked on a worker thread so a big library cannot
// freeze the window. The dialog opens immediately with the database
// side filled and the scan merges in when it lands.
#include "post.h"

#include <QDialog>
#include <QHash>
#include <QSet>
#include <QString>
#include <QVector>

class QTextBrowser;

// One row in a ranked list (tag, artist, booru, …).
struct StatEntry {
    QString label;
    int count = 0;
};

// Statistics for the archive open in the current tab, when that tab is a
// plain local archive (favourites/downloads tabs would double-count).
struct ArchiveStats {
    int posts = 0;
    QHash<QString, int> ratings;
    QVector<StatEntry> topTags;
};

struct StatsReport {
    int favourites = 0;
    int downloadedFiles = 0;   // media files under the download folder
    int downloadedPosts = 0;   // deduped, favourites excluded
    qint64 downloadedBytes = 0;
    int images = 0, gifs = 0, videos = 0;
    int libraryPosts = 0;      // favourites + deduped downloads
    QHash<QString, int> tagCounts; // lowercase tag -> posts using it
    QHash<QString, int> artistCounts;
    QHash<QString, int> characterCounts;
    QHash<QString, int> copyrightCounts;
    QHash<QString, int> ratingCounts;
    QHash<QString, int> booruCounts; // display label -> posts
    // Cache tree (thumbs/, full/, roots/).
    bool cacheScanned = false;
    qint64 cacheBytes = 0;
    qint64 thumbBytes = 0;
    qint64 fullBytes = 0;
    qint64 indexBytes = 0;
    int thumbFiles = 0;
    int fullFiles = 0;
    int indexFiles = 0;
    int cacheLimitMB = 0;
    int cachedTagRows = 0; // rows in tags.db (omnibar vocabulary)
    ArchiveStats archive;
};

struct StatsScanInput {
    QString downloadDir;
    QString cacheDir;
    QSet<QString> favouriteIds; // stable post keys already in favourites
    // Registry lookups are resolved on the UI thread and handed to the
    // worker: no static booru registry is read off-thread.
    QHash<QString, QString> folderKeys;   // download sub-folder -> site key
    QHash<QString, QString> folderLabels; // download sub-folder -> label
    QHash<QString, QString> labelKeys;    // lowercase label/short/key -> key
    QHash<QString, QString> keyLabels;    // site key -> display label
};

struct StatsScanResult {
    int downloadedFiles = 0;
    int downloadedPosts = 0;
    qint64 downloadedBytes = 0;
    int images = 0, gifs = 0, videos = 0;
    QHash<QString, int> tagCounts;
    QHash<QString, int> artistCounts;
    QHash<QString, int> characterCounts;
    QHash<QString, int> copyrightCounts;
    QHash<QString, int> ratingCounts;
    QHash<QString, int> booruCounts;
    bool cacheScanned = false;
    qint64 cacheBytes = 0;
    qint64 thumbBytes = 0;
    qint64 fullBytes = 0;
    qint64 indexBytes = 0;
    int thumbFiles = 0;
    int fullFiles = 0;
    int indexFiles = 0;
};

// Favourites rows -> base report (no filesystem access).
StatsReport statsFromFavourites(const QVector<Post> &favourites);
// Stable identity per post, shared with the download-folder scan so a
// downloaded favourite is not counted twice.
QSet<QString> statsPostKeys(const QVector<Post> &posts);
// Worker-thread collection: download folder + cache trees. Safe to call
// without a QObject; never touches UI or SQLite.
StatsScanResult scanStatsPaths(const StatsScanInput &input);
// Sort a tag/folder tally by count (desc) then label; truncate to limit.
QVector<StatEntry> statsTopEntries(const QHash<QString, int> &counts,
                                   int limit);

// Popup window: cards + ranked lists rendered into a scrollable browser.
class StatsDialog : public QDialog {
    Q_OBJECT
public:
    explicit StatsDialog(const StatsReport &report, QWidget *parent = nullptr);
    void mergeScan(const StatsScanResult &scan);

private:
    void rebuild();
    QString buildHtml() const;

    StatsReport m_report;
    QTextBrowser *m_view = nullptr;
    bool m_scanning = true;
};
