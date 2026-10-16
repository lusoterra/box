#pragma once

#include "flowlayout.h"
#include "favdb.h"
#include "index.h"
#include "remotesource.h"
#include "settings.h"
#include "sidecarrefresh.h"
#include "faviconcache.h"
#include "tagsuggest.h"

#include <QHash>
#include <QImage>
#include <QListView>
#include <QMainWindow>
#include <QPointer>
#include <QPixmap>
#include <QProgressBar>
#include <QStandardItemModel>
#include <QStringList>
#include <QStyledItemDelegate>
#include <QVector>

class QComboBox;
class QDialog;
class QCompleter;
class QDrag;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QStackedWidget;
class QStringListModel;
class QStandardItemModel;
class QSplitter;
class QTimer;
class TagTypeFetcher;
class MarqueeTabBar;
class RemoteSource;
class PostRefresher;
struct SiteCreds;
class DownloadManager;
class ImageCache;

// One tab = one session: a local archive or a remote multi-booru search.
// UI widgets are shared; switching tabs saves/restores this state.
struct BrowserTab {
    QString title;
    bool isRemote = false;
    bool isFavourites = false;
    QString root;              // local
    QStringList sites;         // remote site keys
    QStringList activeTags;
    QString searchQuery;
    QString rating = QStringLiteral("all");
    QString category;
    QString sort = QStringLiteral("id-desc");
    int page = 1;              // remote (highest loaded)
    QVector<Post> localPosts;  // local index cache (+ favourites list)
    QVector<Post> browseMerged; // remote fetched (unfiltered)
};
class ThumbLoader;
class VideoThumbGrabber;
class ViewerPage;
class WelcomePage;

// Grid delegate: uniform-aspect thumbnail + one info card per entry
// ([G] +12 #11984812) + filename line.
class GalleryDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    explicit GalleryDelegate(QObject *parent = nullptr);
    void setAspect(int num, int den);
    void setBoxW(int w) { m_boxW = qBound(96, w, 384); }
    int boxW() const { return m_boxW; }
    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option,
                   const QModelIndex &index) const override;

private:
    int m_num = 1;
    int m_den = 1;
    int m_boxW = 184; // default grid width (kBoxW in mainwindow.cpp)
};

// Grid with drag-out: drag thumbnails to other applications as file URLs.
class GridView : public QListView {
    Q_OBJECT
public:
    explicit GridView(QWidget *parent = nullptr);
    void refreshLayout() { scheduleDelayedItemsLayout(); }
    void setScrollRows(double r) { m_scrollRows = qBound(0.1, r, 3.0); }

protected:
    void startDrag(Qt::DropActions supportedActions) override;
    void wheelEvent(QWheelEvent *e) override;

private:
    int rowPitch() const; // visual row height in px (item + spacing)
    double m_scrollRows = 0.5; // grid rows per wheel notch (0.1-3.0)
    double m_wheelPxAccum = 0.0; // fractional scroll carried across events
};

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(const QString &root, bool rootExplicit = true,
                        QWidget *parent = nullptr);

protected:
    void closeEvent(QCloseEvent *e) override;
    bool eventFilter(QObject *o, QEvent *e) override;

private slots:
    void openFolder();
    void openPath(const QString &path);
    void showWelcome();
    void openPreferences();
    void applyPreferences(const AppSettings &app);
    void offerRestartForLanguage(const QString &previous);
    void showAbout();
    void runClassifyPopup();
    void reload();
    void relinkFavourites(); // match unlinked favs to files on disk
    void onClassifyProgress(int done, int total);
    bool classifyUiTick(int done, int total); // progress UI throttle
    // Waterfall: streaming verdicts land here, accumulate, and flush to
    // the sidebar on a throttle timer (see m_partialTimer).
    void onClassifyPartial(const QHash<QString, QString> &buckets);
    void onBrowseClassifyPartial(const QHash<QString, QString> &buckets);
    void flushClassifyPartials();
    void onClassifyFinished(const QHash<QString, QString> &buckets);
    void onManualFetchDone(const QHash<QString, QString> &buckets);
    void startManualClassify(); // (re)run for popup target + tag verdict DB
    void applyManualBuckets(BrowserTab *t, const QHash<QString, QString> &buckets,
                            bool autoVerdicts);
    void persistFavBuckets(const QVector<Post> &posts); // tag splits -> favdb
    void maybeClassifyBrowse(BrowserTab *t);
    void onBrowseClassifyFinished(const QHash<QString, QString> &buckets);
    void onTabChanged(int index);
    void onTabClosed(int index);
    void saveTabs();   // persist open tabs for session restore
    void stageTabs();  // in-memory snapshot; the ini write is coalesced
    void flushSavedTabs(); // immediate settings.ini write (quit/restart)
    // Skeleton-first activation: activateTab paints the switch now,
    // finishActivateTab rebuilds content on the next event-loop turn.
    void finishActivateTab(int tabIdx);
    bool restoreTabs(); // "last session" startup; false when nothing saved
    void clearStatusProbe(); // drop the clickable error diagnostics
    void setStatusProbe(const QString &title, const QString &text);
    void onSitesChanged();
    void syncSitesMenu();
    void rebuildSitesMenu(); // configured boorus changed (prefs)
    void prefetchFavicons(); // queue site icons after registry changes
    void refreshFavicons();  // drop cached icons and fetch them again
    void showPlusMenu();     // + button: home, specials, booru picker
    void updateDownloadSidecars(); // refresh every downloaded sidecar
    // Bulk refresh worker for an explicit site scope (progress + rescan).
    void runSidecarJobs(const QString &dest, const QSet<QString> &scope,
                        QList<SidecarRefresh::Job> jobs);
    void applyBooruCreds();  // classifier + notes fetchers follow the registry
    void loadNextPage();
    void onViewerPastEnd();
    void onRemoteFinished();
    void toggleFavourite(const Post &post);
    void openFavTab();
    void openLastPlace(); // home resume button: reopen the last live tab
    QString describeLastPlace() const; // short label for the resume button
    void refreshFavTabs(); // re-list the DB into every open fav tab
    void showGridMenu(const QPoint &pos); // right-click actions on posts
    void maybeDeleteUnfavFile(const Post &post); // unfav-deletes-file option
    void importLosnBackup();
    void openStrayDialog();
    void setAsMascot(const Post &post); // home mascot from a grid file
    void importStrayFiles(const QStringList &paths);
    void editStrayTags(const Post &post);
    void refreshPost(const Post &post);
    QString refreshReason(const Post &post);
    void onRefreshDone(qint64 reqId, const QJsonObject &raw);
    void onRefreshFailed(qint64 reqId, const QString &error);
    void loadStylesheet();
    void applyFilter();
    void onSearchEdited(const QString &text);
    void onRemoteSuggest(const QString &siteKey, const QString &prefix,
                         QVector<TagSuggestion> suggestions);
    void fillSuggestModel(const QStringList &tags, const QStringList &buckets);
    void onFaviconReady(const QString &key, QIcon icon);
    void onSearchReturn();
    void onCompleterActivated(const QString &text);
    void removeQueryGroup(int gi); // drop one OR-group card, keep the rest
    static QString canonicalQuery(const QStringList &andTokens,
                                  const QList<QStringList> &groups);
    void onRatingChanged();
    void onCategoryChanged();
    void onAspectChanged();
    void onTagClicked();
    void onGridActivated(const QModelIndex &index);
    void onThumbnailReady(const QString &key, const QImage &image);
    void onScanFinished();
    void onScanProgress(int n, int total);
    void onCacheStats(int hits, int misses);
    void requestVisibleThumbs();
    void backToGrid();

private:
    void startScan(const QString &root);
    void cancelScan(); // stop any running archive scan (quit, rescan)
    bool maybeClassify(bool force = false); // true when a fetch started
    void applyThemeFromSettings();
    void setAspect(int num, int den);
    void setBoxW(int w); // grid post size from the size slider
    void rebuildSidebar();
    // Tabs
    void saveCurrentTab();
    void activateTab(int tabIdx);
    void addLocalTab(const QString &root);
    void addRemoteTab(const QStringList &sites, const QStringList &tags);
    void updateTabTitle(int tabIdx);
    void updateDots(); // · separators only between two shown strings
    // User-visible site names for keys (custom boorus show their label,
    // never the "newbooru" slug). Virtual sources included.
    QStringList siteDisplayNames(const QStringList &sites) const;
    // Browse
    bool isBrowse() const;
    BrowserTab *currentTab();
    void doBrowseSearch(bool append = false);
    void pumpSiteSearch(const QString &key); // next queued OR-alternative
    void applyBrowseFilter();
    void sortPosts(QVector<Post> &posts, const QString &sort);
    void fillModel();
    void rebuildBrowseSidebar();
    void setBrowseUi(bool browse);
    RemoteSource *sourceFor(const QString &key);
    struct SiteCreds credsFor(const QString &key) const;
    void downloadPosts(const QVector<Post> &posts);
    void downloadSelected();
    void showDlDialog();
    void updateDownloadButton();
    void rebuildCombos();
    void rebuildTagTab(QListWidget *list, const QVector<QPair<QString, int>> &entries);
    void commitSearch();
    void commitFilter();
    void saveTabState();
    void updateResultLabel();
    void showViewer(int row);

    QString m_root;
    Index m_index;
    QVector<Post> m_current; // last filtered result (full list, no pages)
    AppSettings m_settings;

    QStringList m_activeTags;
    QString m_searchQuery;
    QString m_rating = QStringLiteral("all");
    QString m_category;
    QString m_sort = QStringLiteral("id-desc");
    int m_aspectNum = 1;
    int m_aspectDen = 1;

    // UI
    QLineEdit *m_searchEdit = nullptr;
    QCompleter *m_completer = nullptr;
    QStandardItemModel *m_completeModel = nullptr; // DisplayRole tag, UserRole bucket
    TagSuggester *m_suggester = nullptr;
    FaviconCache *m_favicons = nullptr;
    // One icon per site, side by side (titled tabs show the query text,
    // untagged tabs fall back to "booru1 + booru2" text + these marks).
    QIcon sitesIcon(const QStringList &sites) const;
    QTimer *m_completeTimer = nullptr;
    QTimer *m_thumbTimer = nullptr;
    QComboBox *m_ratingBox = nullptr;
    QComboBox *m_categoryBox = nullptr;
    QComboBox *m_sortBox = nullptr;
    QComboBox *m_aspectBox = nullptr;
    QSlider *m_sizeSlider = nullptr;
    QLabel *m_countLabel = nullptr;
    QLabel *m_resultLabel = nullptr;
    QLabel *m_dotPage = nullptr;  // separators in the right status cluster
    QLabel *m_dotCount = nullptr;
    QWidget *m_pillBar = nullptr;
    FlowLayout *m_pills = nullptr;
    QProgressBar *m_scanBar = nullptr;
    QListWidget *m_copyList = nullptr;
    QListWidget *m_charList = nullptr;
    QListWidget *m_artistList = nullptr;
    QListWidget *m_tagList = nullptr;
    QListWidget *m_metaList = nullptr;
    QStackedWidget *m_pages = nullptr;
    QStackedWidget *m_stage = nullptr; // 0 = welcome, 1 = browser
    WelcomePage *m_welcome = nullptr;
    QSplitter *m_sideSplit = nullptr; // resizable tag sections
    GridView *m_grid = nullptr;
    ViewerPage *m_viewer = nullptr;
    GalleryDelegate *m_delegate = nullptr;
    QStandardItemModel *m_model = nullptr;
    ThumbLoader *m_thumbs = nullptr;
    VideoThumbGrabber *m_videoThumbs = nullptr;
    TagTypeFetcher *m_fetcher = nullptr;
    TagTypeFetcher *m_browseFetcher = nullptr; // remote untyped tags
    qint64 m_lastClassifyUi = 0; // progress-UI throttle stamp (ms)
    QPointer<QDialog> m_classifyPop; // update-tags progress popup
    BrowserTab *m_manualTab = nullptr; // popup classify target (validated)
    QString m_manualDb;                // tag DB the popup run uses
    bool m_manualActive = false;       // suppress auto finish handlers
    // Tab a browse classify run belongs to (raw: verified against m_tabs
    // on finish, since tabs can close mid-fetch). -1 = local run / none.
    BrowserTab *m_classifyTab = nullptr;
    // Regeneration attempts this session (site#id): favourited strays
    // with a source URL re-download once, then never nag again.
    QSet<QString> m_regenTried;
    // Explicit-unfavourite vetoes (site#pid): an in-flight or future
    // download finishing must not re-favourite the row. Cleared by an
    // explicit favourite or an explicit download request for the post.
    // Session-only: a fresh launch restores the automatic coupling.
    QSet<QString> m_unfavVeto;
    // Coalesced settings.ini flush (tab switches come in bursts; the
    // registry serialization is the slowest part of a switch).
    QTimer *m_saveTimer = nullptr;
    ImageCache *m_imgCache = nullptr;
    DownloadManager *m_downloads = nullptr;
    class DownloadsDialog *m_dlDialog = nullptr;
    // Download destinations written since their tab last scanned
    // (canonical paths): landing on a dirty local tab rescans it, so new
    // files show up without a manual F5. Cleared once consumed.
    QSet<QString> m_dirtyRoots;
    FavDb m_favDb;
    PostRefresher *m_refresher = nullptr;
    // Last browse error with its raw request/response attached. The status
    // bar shows a one-liner; clicking it opens the full inspector.
    QString m_statusProbeTitle;
    QString m_statusProbeText;
    bool m_statusProbeValid = false;
    Post m_refreshPost; // post currently being tag-refreshed
    RefreshTarget m_refreshTarget;
    BrowserTab *m_searchTab = nullptr; // tab that owns the in-flight search
    // Tabs (tabbar index 0 = welcome, m_tabs[i] <-> tabbar i+1)
    MarqueeTabBar *m_tabBar = nullptr;
    QList<BrowserTab *> m_tabs;
    int m_curTab = -1;
    // Browse state
    QHash<QString, RemoteSource *> m_sources;
    QVector<Post> m_browseMerged;
    // Fan-out state for OR-group searches: each site works through its
    // queued AND-queries one at a time; completions accumulate below.
    struct PendingSite {
        BooruEngine engine = BooruEngine::Danbooru;
        QList<QStringList> queriesLeft; // token lists, head is in flight
        QVector<Post> posts;           // raw accumulated results
        QString error;                 // first error text (if any)
        QString probe;                 // its raw diagnostics section
        bool transientError = false;   // any completion looked retryable
    };
    QHash<QString, PendingSite> m_pendingSites;
    // Fetched classify verdicts, applied per host group so buckets a host
    // disables fold to general instead of re-separating (manual overrides
    // bypass this: explicit per-tag intent wins over host taxonomy).
    static int applyHostBuckets(QVector<Post> &work,
                                const QHash<QString, QString> &buckets);
    // Unflushed streaming classify verdicts (waterfall sidebar). Cleared
    // by flushClassifyPartials and by the finished handlers.
    QHash<QString, QString> m_pendingBuckets;
    QHash<QString, QString> m_pendingBrowseBuckets;
    QTimer *m_partialTimer = nullptr; // throttles waterfall sidebar refreshes
    int m_pendingSearches = 0;
    bool m_appendResults = false;
    bool m_searchRetried = false; // one automatic retry per search at most
    bool m_loadingPage = false;
    qint64 m_lastPageLoadMs = 0;
    bool m_advanceOnLoad = false; // viewer asked past the end: go there after load
    int m_advanceRow = 0;
    QPushButton *m_sitesBtn = nullptr;
    QPushButton *m_plusBtn = nullptr;
    QMenu *m_sitesMenu = nullptr;
    QLabel *m_pageLabel = nullptr;
    QPushButton *m_selectBtn = nullptr;
    QPushButton *m_downloadSelBtn = nullptr;
    QPushButton *m_dlBtn = nullptr;
    QProgressBar *m_dlBar = nullptr; // feedbar: quiet download progress
    QTimer *m_dlHide = nullptr;
    bool m_selectMode = false;
    QHash<QString, QPixmap> m_thumbCache; // request key -> pixmap
    QList<QString> m_cacheOrder;
    QPixmap m_placeholder;
    class ScanWatcher;
    class Scanner *m_scanner = nullptr; // current archive scan (cancellable)
    ScanWatcher *m_scanWatcher = nullptr;
};
