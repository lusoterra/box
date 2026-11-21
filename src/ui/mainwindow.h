#pragma once

#include "flowlayout.h"
#include "favdb.h"
#include "index.h"
#include "remotesource.h"
#include "settings.h"
#include "sidecarrefresh.h"
#include "faviconcache.h"
#include "straydialog.h"
#include "tagcountcache.h"
#include "tagsuggest.h"

#include <QFutureWatcher>
#include <QHash>
#include <QImage>
#include <QListView>
#include <QMainWindow>
#include <QPointer>
#include <QPixmap>
#include <QProgressBar>
#include <QSharedPointer>
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
class QScrollArea;
class QStackedWidget;
class QStringListModel;
class QStandardItemModel;
class QSplitter;
class QTimer;
class GridLoadingOverlay;
class LosnImportDialog;
class TagTypeFetcher;
class MarqueeTabBar;
class RemoteSource;
class PostRefresher;
struct SiteCreds;
class DownloadManager;
class ImageCache;
class GalleryDlRunner;
class GalleryDlDialog;

std::pair<QString, qint64> favIdentity(const Post &p);
QString favVetoKey(const QString &site, qint64 pid);
extern const QString kSrcDownloads;
extern const QString kSrcFavourites;
QString viewerKeyFor(const Post &post);
// Map a raw API object through the engine that owns the refresh target.
Post remapFresh(const QJsonObject &raw, const RefreshTarget &t);
bool isVirtualSource(const QString &key);
bool blacklisted(const Post &p, const QSet<QString> &banned);
void splitIncExc(const QStringList &tokens, QStringList &inc, QStringList &exc);
bool matchesTokens(const Post &p, const QStringList &inc, const QStringList &exc);
bool isRatingToken(const QString &tok);
constexpr int kBoxW = 184;
bool groupsMatch(const Post &p, const QList<QStringList> &groups);
QString dedupeKeyFor(const Post &post);

// One tab = one session: a local archive or a remote multi-booru search.
// UI widgets are shared; switching tabs saves/restores this state.
struct BrowserTab {
    QString title;
    bool isRemote = false;
    bool isFavourites = false;
    QString root;              // local
    QStringList sites;         // remote site keys
    QStringList activeTags;
    QSet<QString> droppedDefaults; // entry defaultTags disabled via pill x
    QString searchQuery;
    QString rating = QStringLiteral("all");
    QString category;
    QString sort = QStringLiteral("id-desc");
    // Downloads-only media filter (file-extension based: all/images/gifs/
    // animated/videos). Other tabs ignore it and keep the combo hidden.
    QString mediaFilter = QStringLiteral("all");
    int page = 1;              // remote (highest loaded)
    QVector<Post> localPosts;  // local index cache (+ favourites list)
    QVector<Post> browseMerged; // remote fetched (unfiltered)
    QString viewerKey; // open post identity (site#id or file); empty = grid
    int scrollRow = -1; // first visible grid row (restored on tab/session)
};
class ThumbLoader;
class VideoThumbGrabber;
class ViewerPage;
class WelcomePage;

// Per-card thumbnail progress shown on black cards: queued (waiting for
// a pool thread) or downloading (byte progress; percent < 0 when the
// server sends no Content-Length).
struct ThumbLoadState {
    enum Phase { Idle = 0, Queued = 1, Downloading = 2 };
    int phase = Idle;
    int percent = -1;
};

// Grid delegate: uniform-aspect thumbnail + one info card per entry
// ([G] +12 #11984812) + filename line.
class GalleryDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    explicit GalleryDelegate(QObject *parent = nullptr);
    void setAspect(int num, int den);
    void setBoxW(int w) { m_boxW = qBound(96, w, 384); }
    int boxW() const { return m_boxW; }
    // Brick-staggered rows: odd columns paint half a card lower.
    void setStaggered(bool on) { m_staggered = on; }
    bool staggered() const { return m_staggered; }
    // Live load state (owned by MainWindow) + animation angle for the
    // muted spinner/indeterminate bar over a not-yet-painted card.
    void setLoadStates(const QHash<QString, ThumbLoadState> *states) {
        m_loadStates = states;
    }
    void setSpinAngle(int deg) { m_spinAngle = deg; }
    // Geometry of the filename line under the info card, in viewport
    // coordinates for one item (same math as paint; used for the hover
    // tooltip that shows the untruncated name).
    QRect fileNameRect(const QStyleOptionViewItem &option) const;
    QRect paintRect(const QStyleOptionViewItem &option) const;
    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option,
                   const QModelIndex &index) const override;

private:
    int m_num = 1;
    int m_den = 1;
    int m_boxW = 184; // default grid width (kBoxW in mainwindow.cpp)
    bool m_staggered = false;
    const QHash<QString, ThumbLoadState> *m_loadStates = nullptr;
    int m_spinAngle = 0;
};

// Grid with drag-out: drag thumbnails to other applications as file URLs.
class GridView : public QListView {
    Q_OBJECT
public:
    explicit GridView(QWidget *parent = nullptr);
    void refreshLayout() { scheduleDelayedItemsLayout(); }
    void setScrollRows(double r) { m_scrollRows = qBound(0.1, r, 3.0); }
    int rowPitch() const; // visual row height in px (item + spacing)

signals:
    void viewportResized();

protected:
    void startDrag(Qt::DropActions supportedActions) override;
    bool viewportEvent(QEvent *e) override;
    void wheelEvent(QWheelEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override; // Space toggles selection

private:
    double m_scrollRows = 0.5; // grid rows per wheel notch (0.1-3.0)
    double m_wheelPxAccum = 0.0; // fractional scroll carried across events
};

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(const QString &root, bool rootExplicit = true,
                        QWidget *parent = nullptr);

    // Sequential gallery-dl batch state (one flight per pasted URL so
    // every file attributes to the page it came from). Chain-shared, no
    // cycles: the finished handler either recurses or lets it drop.
    struct GalleryChain {
        QString binary;
        QString dest;
        QStringList all;
        QStringList pending;
        GalleryDlRunner *runner = nullptr;
        GalleryDlDialog *dlg = nullptr;
        int files = 0;
        int sidecars = 0;
        QStringList errors;
        bool cancelled = false;
    };

protected:
    void closeEvent(QCloseEvent *e) override;
    void changeEvent(QEvent *e) override;
    bool eventFilter(QObject *o, QEvent *e) override;

private slots:
    void openFolder();
    void openPath(const QString &path);
    void showWelcome();
    void openPreferences();
    void applyPreferences(const AppSettings &app);
    void offerRestartForLanguage(const QString &previous);
    void showAbout();
    void showStatistics(); // Info menu: library/cache/tag stats popup
    void runClassifyPopup();
    void reload();
    void relinkFavourites(); // match unlinked favs to files on disk
    // Manual re-pull of favourited files deleted outside the app
    // (File menu; moves already healed by relinkFavourites). No schema
    // needed: local_path is the was-downloaded flag, the fav row the
    // isFavourite.
    void redownloadMissingFiles();
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
    void closeCurrentTab();   // Ctrl+W
    void openDefaultTab();    // Ctrl+T: Default tab booru / default boorus
    void reopenClosedTab();   // Ctrl+Shift+T (LIFO closed-tab stack)
    void downloadCurrentOrSelection(); // Ctrl+D
    void focusOmniBar();      // Ctrl+L (leaves the viewer first)
    void openCurrentPostInBrowser();    // Ctrl+O
    void openCurrentPostExternally();   // Ctrl+Shift+O
    void showKeyboardHelp();  // F1 / Help menu
    void showViewerTagMenu(const QString &tag, const QPoint &globalPos);
    void onTabMoved(int from, int to); // drag-reorder: keep m_tabs parallel
    void saveTabs();   // persist open tabs (coalesced; for switches/edits)
    void saveTabsNow(); // same, but synchronous (tab add/close/move: no crash window)
    void stageTabs();  // in-memory snapshot; the ini write is coalesced
    void flushSavedTabs(); // immediate settings.ini write (quit/restart)
    // Skeleton-first activation: activateTab paints the switch now,
    // finishActivateTab rebuilds content on the next event-loop turn.
    void finishActivateTab(int tabIdx);
    // Drop the activation veil once content lands. `owner` is the tab the
    // finishing work belongs to: a stale scan/search from a tab the user
    // already left must not clear the new tab's busy state.
    void hideTabLoading(const BrowserTab *owner);
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
    void openLastPlace(); // home resume button: restore the last session
    void startFreshSession(); // close every tab, forget the session, go home
    QString describeLastPlace() const; // short label for the resume button
    void refreshFavTabs(); // re-list the DB into every open fav tab
    // Download links (site, id) -> local_path, inverted: local scanned
    // posts carry ("local", file-hash) identities that never match their
    // ("Site", id) favourite rows, so grid fav/delete actions resolve by
    // canonical path instead of identity alone.
    QHash<QString, QList<QPair<QString, qint64>>> favKeysByPath(
        const QHash<QPair<QString, qint64>, QString> &recorded) const;
    // Every DB identity a grid post answers to: its own, plus the
    // recorded download links behind its path.
    QList<QPair<QString, qint64>>
    favKeysFor(const Post &p,
               const QHash<QString, QList<QPair<QString, qint64>>> &byPath) const;
    // A grid post counts as favourited when any of its identities has a
    // row (covers Downloads cards behind ("Site", id) rows).
    bool isFavMarked(const Post &p,
                     const QHash<QPair<QString, qint64>, QString> &favLocal,
                     const QHash<QString, QList<QPair<QString, qint64>>> &byPath) const;
    // Recompute the current tab's favourite/download marks in place (bulk
    // fav/unfav must not wait for the next refill) and refresh the viewer
    // badges.
    void repaintFavMarks();
    // Viewer-fetched notes/comments OR into the grid pills in place.
    void onMetaMarksChanged(const QString &siteKey, qint64 postId,
                            bool hasComments, bool hasNotes);
    void showGridMenu(const QPoint &pos); // right-click actions on posts
    void maybeDeleteUnfavFile(const Post &post); // unfav-deletes-file option
    void importLosnBackup();
    void exportLibraryCsv(); // File menu: favourites + downloads -> CSV
    void importLibraryCsv(); // File menu: restore a CSV backup
    void launchImportDialog(LosnImportDialog *dlg); // shared post-launch wiring
    void exportSettingsBackup(); // File menu: settings+favs -> JSON file
    void importSettingsBackup(); // File menu: merge a JSON backup back in
    void openStrayDialog();
    void setAsMascot(const Post &post); // home mascot from a grid file
    void runGalleryDl(const QStringList &urls);
    void startGalleryFlight(QSharedPointer<GalleryChain> chain);
    void importStrayFiles(const QStringList &paths);
    void editStrayTags(const Post &post);
    void applyStrayTagEdits(const QVector<StrayTagEdit> &edits);
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
    // Empty omnibar: the popup becomes the favourite-tag picker (ranked by
    // post count, no network).
    void showFavSuggestions();
    // Mouse pick: append the tag as a query token, keep the popup open.
    void acceptSuggestion(const QString &tag);
    // Seed the persistent tag cache with the archive vocabulary.
    void syncTagCacheFromIndex();
    void onFaviconReady(const QString &key, QIcon icon);
    void onSearchReturn();
    // Append committed query tokens as pills: negated tokens flip the
    // matching positive pill, repeats are ignored.
    void mergeActiveTags(const QStringList &tokens);
    void onCompleterActivated(const QString &text);
    void removeQueryGroup(int gi); // drop one OR-group card, keep the rest
    static QString canonicalQuery(const QStringList &andTokens,
                                  const QList<QStringList> &groups);
    void onRatingChanged();
    void onCategoryChanged();
    void onMediaChanged();
    // Media-kind box follows the committed filetype: tags (single-select
    // shortcut like the rating box): exact token match selects, else All.
    void syncMediaBox();
    void normalizeTypedFiletype();
    void onTagClicked();
    void onTagContextMenu(const QPoint &pos); // sidebar right-click: fav toggle
    // Starred tags (lowercased) for suggestion priority + tag menus.
    QSet<QString> favTagSet() const;
    bool isFavTag(const QString &tag) const;
    void toggleFavTag(const QString &tag);
    // HyBooru summaries carry no tags: hydrate the loaded page in the
    // background so the sidebar fills like every other engine.
    void scheduleHybooruHydration();
    void pumpHybooruHydration(); // omnibar suggestions: click adds to search
    void onGridActivated(const QModelIndex &index);
    void onThumbnailReady(const QString &key, const QImage &image);
    void onThumbQueued(const QString &key);
    void onThumbStarted(const QString &key);
    void onThumbProgress(const QString &key, qint64 received, qint64 total);
    void onThumbFailed(const QString &key);
    // Repaint only the visible cards in m_thumbLoad (spinner/progress).
    void updateThumbLoadPaint();
    void syncThumbSpinTimer();
    // A grid thumbnail hit a challenge wall: register the media wall and
    // start one capture pass per site (not one dialog per black card).
    void onThumbWall(const QString &siteKey, const QString &url);
    void onScanFinished(const QString &scannedRoot,
                        const QVector<Post> &scanned);
    void onScanProgress(int n, int total);
    void onCacheStats(int hits, int misses);
    void requestVisibleThumbs();
    // Visible model rows via item geometry (indexAt misses IconMode
    // margins/ragged edges). Drives thumb requests + pixmap windowing.
    void visibleRowRange(int &lo, int &hi) const;
    void backToGrid(bool keepViewerState = false);
    // Fullscreen post view: shell chrome + viewer toolbar hide, media fills.
    void toggleViewerFullscreen();
    void setViewerFullscreen(bool on);
    bool viewerFullscreen() const { return m_viewerFullscreen; }
    // Right-click menu for one post (grid card or media surface).
    void showViewerContextMenu(const Post &post, const QPoint &globalPos);
    // Metadata sidecar for a post (local media, downloaded favourite) or
    // empty when only the in-memory API object exists.
    QString postJsonSidecar(const Post &post);
    // Summary + raw JSON dialog (grid/viewer context menu).
    void showPostMetadata(const Post &post);

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
    // True for tabs backed by the download folder: the local `Browse
    // Downloads` tab and browse tabs whose sources include the virtual
    // `Downloads` entry.
    bool isDownloadsTab();
    BrowserTab *currentTab();
    void doBrowseSearch(bool append = false);
    void pumpSiteSearch(const QString &key); // next queued OR-alternative
    // Captcha/PoW wall menu for wall-blocked sites: solve in the capture
    // browser, or drop the booru (deselect in multibooru, close tab solo).
    void showWallMenu(const QStringList &wallSites);
    void solveWallInBrowser(const QString &siteKey,
                                const QString &wallKind = {},
                                const QUrl &startUrl = {});
    // Start page for the solve browser: the failing request URL when the
    // wall was observed, else a media-wall file URL, else empty (homepage).
    QUrl wallStartUrl(const QString &siteKey) const;
    void dropWallSites(const QStringList &siteKeys);
    void applyBrowseFilter();
    void sortPosts(QVector<Post> &posts, const QString &sort);
    void fillModel();
    void rebuildBrowseSidebar();
    // Fill in missing booru post URLs on scanned downloads (site + id ->
    // post page), except strays. Runs on the UI thread after each scan.
    void healLocalPosts(QVector<Post> &posts);
    // True when the media actually exists under the download folder:
    // local posts by path, remote posts by content md5 (or the planned
    // host-folder filename when the post has no hash). Purely filesystem
    // based, so stale DB local_paths never fake or hide a download badge.
    bool isPostDownloaded(const Post &post);
    // Download-folder scan backing isPostDownloaded(): md5 stems and
    // complete file stems. Rescans when the folder's mtime changes or
    // invalidateDownloadIndex() says so.
    struct DownloadedIndex {
        QSet<QString> md5s;
        QSet<QString> stems;
        QString root;
        qint64 stamp = -1;
    };
    const DownloadedIndex &downloadDiskIndex();
    void invalidateDownloadIndex() { m_dlIndex.stamp = -1; }
    // Refresh the viewer AI/favourite/downloaded marks for its current
    // post (call after fav/download changes while the viewer is open).
    void updateViewerBadges();
    // Re-point an open viewer at the rebuilt grid (see fillModel).
    void syncViewerToGrid();
    void setBrowseUi(bool browse);
    RemoteSource *sourceFor(const QString &key);
    struct SiteCreds credsFor(const QString &key) const;
    // confirmBulk=false for callers that already asked (e.g. redownload
    // missing files); every multi-post request otherwise gets a warning.
    void downloadPosts(const QVector<Post> &posts, bool confirmBulk = true);
    void addTagToSearch(const QString &tag); // viewer tag click/menu
    void searchVariants(const Post &post); // parent/child family search
    void downloadSelected();
    // Keyboard targets: viewer post when open, else the grid selection,
    // else the current grid row (empty when nothing is available).
    QVector<Post> keyboardTargets() const;
    // Files for a menu action: local copies straight from disk, remote
    // rows fetched into the shared temp folder (TempMedia) first.
    void copyPostFilesToClipboard(const QVector<Post> &posts);
    void showDlDialog();
    // Quit/startup download guards: warn before dropping a batch, and
    // offer to continue a journal left by a previous session.
    void showDownloadQuitPrompt();
    void maybeResumeDownloads();
    void updateDownloadButton();
    void rebuildCombos();
    // Sidebar section fill: entries render in the section's bucket colour
    // (same palette as the filter pills and the suggest popup).
    void rebuildTagTab(QListWidget *list, const QVector<QPair<QString, int>> &entries,
                       const QString &bucket,
                       const QString &sourceCategory = QString());
    void commitSearch();
    void commitFilter();
    // Typed rating:/safety: tags own the filter (the rating box is only a
    // shortcut): a committed typed token clears the box to "All" so the
    // two can never AND each other into an empty grid.
    void normalizeTypedRating();
    void saveTabState();
    void updateResultLabel();
    // Sort-box enablement follows the tab: order-preserving sorts are
    // always on; the rest need every tab site to translate them
    // server-side (else infinite scroll re-sorts on every append).
    void updateSortBox();
    void showViewer(int row);
    bool m_viewerFullscreen = false;
    Qt::WindowStates m_preFullscreenState = Qt::WindowNoState;
    bool m_preFullscreenMaximized = false;
    // Set while a fullscreen exit is restoring a maximized window. KWin
    // answers the unfullscreen by restoring the last normal geometry and
    // dropping the maximize flag, so the request has to be repeated once
    // the compositor's configure has landed (changeEvent).
    bool m_pendingFullscreenMaximize = false;

    QString m_root;
    Index m_index;
    QVector<Post> m_current; // last filtered result (full list, no pages)
    AppSettings m_settings;

    QStringList m_activeTags;
    QString m_searchQuery;
    QString m_rating = QStringLiteral("all");
    QString m_category;
    QString m_sort = QStringLiteral("id-desc");
    int m_requestedBoxW = 184; // user-requested size (pre-snap)
    void applyGridSnap();      // fill the viewport when gridSnap is on
    int m_aspectNum = 1;
    int m_aspectDen = 1;

    // UI
    QWidget *m_omniBar = nullptr; // search row: hidden in post view
    QLineEdit *m_searchEdit = nullptr;
    QCompleter *m_completer = nullptr;
    QStandardItemModel *m_completeModel = nullptr; // DisplayRole tag, UserRole bucket
    TagSuggester *m_suggester = nullptr;
    // Persistent tag vocabulary (tag + bucket + post count) learned from
    // tag-API answers: instant, popularity-ranked omnibar answers.
    TagCountCache m_tagCache;
    FaviconCache *m_favicons = nullptr;
    // One icon per site, side by side (titled tabs show the query text,
    // untagged tabs fall back to "booru1 + booru2" text + these marks).
    QIcon sitesIcon(const QStringList &sites) const;
    QTimer *m_completeTimer = nullptr;
    QTimer *m_thumbTimer = nullptr;
    QComboBox *m_ratingBox = nullptr;
    QComboBox *m_categoryBox = nullptr;
    QComboBox *m_mediaBox = nullptr; // downloads-only extension filter
    QComboBox *m_sortBox = nullptr;
    QSlider *m_sizeSlider = nullptr;
    QLabel *m_countLabel = nullptr;
    QLabel *m_resultLabel = nullptr;
    QLabel *m_dotPage = nullptr;  // separators in the right status cluster
    QLabel *m_dotCount = nullptr;
    QWidget *m_pillBar = nullptr;
    QWidget *m_sideBar = nullptr; // sidebar column (hidden in fullscreen)
    QLabel *m_sideBusy = nullptr; // "Loading tags…" while a tab switches
    // Swap the sidebar between the busy placeholder and the real sections.
    void setSidebarLoading(bool busy);
    FlowLayout *m_pills = nullptr;
    QProgressBar *m_scanBar = nullptr;
    GridLoadingOverlay *m_gridLoading = nullptr; // tab-activation busy veil
    QListWidget *m_copyList = nullptr;
    QListWidget *m_charList = nullptr;
    QListWidget *m_artistList = nullptr;
    QListWidget *m_tagList = nullptr;
    QListWidget *m_metaList = nullptr;
    // Dynamic browse-sidebar sections for native custom categories
    // (Nuuru flags, Szuru categories…): fixed five keep indices 0-4,
    // these append after. Hidden when their category leaves the page.
    struct CustomSidebarSection {
        QWidget *box = nullptr;
        QLabel *title = nullptr;
        QListWidget *list = nullptr;
    };
    QMap<QString, CustomSidebarSection> m_customSections;
    QList<QLabel *> m_sideHeaders; // fixed 5, same order as m_sideSplit
    QStackedWidget *m_pages = nullptr;
    QStackedWidget *m_stage = nullptr; // 0 = welcome, 1 = browser
    WelcomePage *m_welcome = nullptr;
    QSplitter *m_sideSplit = nullptr; // resizable tag sections
    QScrollArea *m_sideScroll = nullptr; // wraps splitter in single-scroll mode
    void applySidebarScrollMode(); // per-category vs one-big-scrollbar
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
    // relinkFavourites stats every linked file and can walk the whole
    // download tree: once per launch is enough (downloads/strays store
    // their new paths directly, imports link their own rows).
    bool m_favRelinked = false;
    // Download-folder inventory for the download badge (see downloadDiskIndex).
    DownloadedIndex m_dlIndex;
    // Explicit-unfavourite vetoes (site#pid): an in-flight or future
    // download finishing must not re-favourite the row. Cleared by an
    // explicit favourite or an explicit download request for the post.
    // Session-only: a fresh launch restores the automatic coupling.
    QSet<QString> m_unfavVeto;
    // Media wall auto-heals already attempted (siteKey#id, session-only):
    // one silent clearance re-ride per post, then the error stands.
    QSet<QString> m_wallRefreshed;
    // Coalesced settings.ini flush (tab switches come in bursts; the
    // registry serialization is the slowest part of a switch).
    QTimer *m_saveTimer = nullptr;
    ImageCache *m_imgCache = nullptr;
    PostRefresher *m_refresher = nullptr;
    PostRefresher *m_hybooruFetch = nullptr;
    QVector<Post> m_hybooruQueue;
    QSet<QString> m_hybooruTried;
    bool m_hybooruBusy = false;
    bool m_hybooruDeferring = false;
    RefreshTarget m_hybooruTarget;
    Post m_hybooruCurrent;
    DownloadManager *m_downloads = nullptr;
    class DownloadsDialog *m_dlDialog = nullptr;
    // Close flow: set when the quit prompt was already answered (or a
    // drain was requested), so the second close() goes straight through.
    bool m_quitBypassPrompt = false;
    bool m_quitWhenDownloadsDone = false;
    // Download destinations written since their tab last scanned
    // (canonical paths): landing on a dirty local tab rescans it, so new
    // files show up without a manual F5. Cleared once consumed.
    QSet<QString> m_dirtyRoots;
    FavDb m_favDb;
    // Last browse error with its raw request/response attached. The status
    // bar shows a one-liner; clicking it opens the full inspector.
    QString m_statusProbeTitle;
    QString m_statusProbeText;
    bool m_statusProbeValid = false;
    QPushButton *m_wallSolveBtn = nullptr; // status-bar one-click wall solver
    QStringList m_wallSites; // walled sites from the last finished search
    QHash<QString, QString> m_wallKinds; // site key -> pow/captcha/challenge
    QHash<QString, QUrl> m_wallFailUrls; // site key -> failing request URL
    // Media walls (downloaded HTML instead of bytes): (siteKey, file URL).
    // Kept apart from search walls (no auto-popup, no drop/close actions);
    // the Solve button covers the union of both.
    QList<QPair<QString, QString>> m_mediaWalls;
    QHash<QString, qint64> m_lastWallPopup; // site key -> last auto-popup
                                           // (loop breaker: a solve that
                                           // doesn't stick must not summon
                                           // the browser on every retry)
    void updateWallButton(); // visibility + label follow m_wallSites
    void solveWallFromButton(); // first site, or a menu when several
    Post m_refreshPost; // post currently being tag-refreshed
    RefreshTarget m_refreshTarget;
    qint64 m_refreshReq = 0; // refresh generation: stale completions drop
    BrowserTab *m_searchTab = nullptr; // tab that owns the in-flight search
    // Tabs (tabbar index 0 = welcome, m_tabs[i] <-> tabbar i+1)
    MarqueeTabBar *m_tabBar = nullptr;
    QList<BrowserTab *> m_tabs;
    // Ctrl+Shift+T history: newest first, capped (browser-like reopen).
    // Heavy rows are dropped on close; activation rescans/re-searches.
    QList<BrowserTab *> m_closedTabs;
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
        bool wall = false; // any completion died on a captcha/PoW wall
        QString wallKind; // pow | captcha | challenge (first wins)
        QUrl failUrl;   // exact request URL that walled (dialog start page)
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
    QPushButton *m_downloadSelBtn = nullptr;
    QPushButton *m_dlBtn = nullptr;
    QProgressBar *m_dlBar = nullptr; // feedbar: quiet download progress
    QTimer *m_dlHide = nullptr;
    QHash<QString, QPixmap> m_thumbCache; // request key -> pixmap
    QList<QString> m_cacheOrder;
    // Per-card veil state + a calm animation tick shared by all cards.
    QHash<QString, ThumbLoadState> m_thumbLoad;
    QTimer *m_thumbSpinTimer = nullptr;
    int m_thumbSpinAngle = 0;
    QPixmap m_placeholder;
    class ScanWatcher : public QFutureWatcher<QVector<Post>> {};
    class Scanner *m_scanner = nullptr; // current archive scan (cancellable)
    ScanWatcher *m_scanWatcher = nullptr;
};
