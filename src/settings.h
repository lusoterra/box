#pragma once

// Persistent options (~/.config/box/settings.ini).
// Single source of truth: defaults live in AppSettings, the preferences
// dialog edits a copy, MainWindow applies + saves. Single set of
// settings + one data dir (LoliSnatcher-style).
#include <QByteArray>
#include <QColor>
#include <QDir>
#include <QList>
#include <QMap>
#include <QStandardPaths>
#include <QString>
#include <QStringList>

#include "explicittags.h"
#include "remotesource.h" // BooruSite: user-configured booru list
#include "tagcategories.h" // per-booru category merge/rename rules

struct AppSettings {
    // Saved local archive locations (Preferences -> Library). Each entry is
    // a read-only archive root the "+" menu can open in its own tab; they
    // never share tabs, filters or scan caches with each other.
    struct SavedArchive {
        QString name; // display label ("Archive A"); empty = folder name
        QString root;
        bool operator==(const SavedArchive &) const = default;
    };
    // Library
    bool indexCache = true;
    // Duplicate cleanup: allow deleting files inside read-only local
    // archive roots (Preferences -> Library). Off by default; downloads
    // and Hydrus duplicates can still be removed when off.
    bool dedupAllowArchiveDelete = false;
    // On-disk media cache policy (thumbnails + full-quality browsing
    // cache). "Nothing" keeps only in-memory/session data; explicit
    // downloads are never affected. "SFW only" still caches explicitly
    // downloaded media (Post::downloaded), never favourites-only rows.
    // Per-booru entries can narrow this further (BooruSite::cacheMode).
    CacheMode cacheMode = CacheMode::All;
    // Full-quality image cache while browsing. OFF by default: silent full
    // downloads would fill the disk with images you never asked to keep.
    // It is also subject to cacheMode (and any per-booru override).
    bool fullCache = false;
    // What dragging a remote post hands to other apps. Per-booru entries
    // can override this (BooruSite::dragLinkMode).
    DragLinkMode dragLinkMode = DragLinkMode::PostPage;
    // Total cache cap in MB (thumbs + full images). 0 = unlimited.
    int cacheLimitMB = 10240;
    // Optional override for the on-disk cache root; empty = default
    // (~/.cache/box via QStandardPaths::CacheLocation).
    QString cacheDir;
    // Cache media opened from the favourites tab too (downloads already
    // are). Off keeps fav views session-only.
    bool cacheFavs = true;
    int thumbLongSide = 360; // 256/360/512/768
    QString lastRoot;
    QStringList history; // most-recent-first archive roots, cap 10
    QList<SavedArchive> archives; // named local archives for the "+" menu
    QStringList historyTokens; // recent search tokens, cap 200
    bool restoreSession = false; // legacy: migrated to tabsDefault="last"
    QString language; // UI locale override ("fr", "pt_BR"); empty = system
    // Favourites (DB entries, no files) + optional coupling with downloads.
    // Each direction is its own toggle: favouriting can download, downloads
    // can favourite, unfavouriting can delete, deleting can unfavourite.
    bool favAutoDownload = false; // favouriting also downloads full quality
    bool favAutoFav = true;       // downloads also get favourited
    // Unfavouriting also deletes the downloaded file. On by default.
    bool unfavDeletesFile = true;
    // Deleting a file also removes the favourite entry. Off by default:
    // Delete stays a disk-only affair unless this is on.
    bool deleteUnfavs = false;
    // Strays pulled in via File -> Download/Add Stray get favourited
    // automatically. On by default (they have no home tab otherwise).
    bool strayAutoFav = true;
    // Read-only Hydrus tag lookups and the isolated Hydrus tab
    // (Preferences → Hydrus): two independent switches. Lookups only need
    // a local hydd / Hydrus client API (or a reverse proxy in front of
    // one); the tab adds its own library and media storage. Disabled by
    // default; the app never writes tags back to Hydrus.
    bool hydrusTagLookups = false;
    bool hydrusTab = false;
    QString hydrusUrl = QStringLiteral("http://127.0.0.1:45869");
    QString hydrusAccessKey;
    // Optional HTTP/SOCKS proxy for Hydrus traffic; empty = direct for
    // loopback URLs, global proxy otherwise.
    QString hydrusProxy;
    // Hydrus tab storage, isolated from the booru library: a media folder
    // and its own hydrus.db. Empty = <data>/hydrus and <data>/hydrus.db.
    QString hydrusDir;
    QString hydrusDb;
    // Tag masking for automatic Hydrus tags: drop tags whose hydd
    // network-wide mapping count is below this (0 = keep everything).
    // Needs hydd /hyd/tag_counts; without it, all tags are kept and the
    // tab explains why. Manual edits are never masked.
    int hydrusMinTagPosts = 10;
    bool hydrusDropUnknownTags = false;
    // Read-only LAN sharing (File -> Share library on LAN…): bind/port and
    // the last selected sources. Sources are picked again on each start.
    QString shareBind = QStringLiteral("0.0.0.0");
    int sharePort = 7071;
    bool shareHydrus = false;
    bool shareDownloads = false;
    bool shareFavourites = false;
    bool shareLocalFolder = false;
    QString shareFolder;
    // Blacklisted tags: hidden everywhere (local + browse).
    QStringList blacklist;
    // Ugoira downloads: converted from the served .zip to an animated
    // file instead ("webp" | "jxl" | "zip" = keep original). Never gif.
    QString ugoiraFormat = QStringLiteral("webp");
    // Per-frame delay ms when the API omits pixiv frame data.
    // Fallback per-frame delay when a zip carries no animation.json:
    // 83 ms = 12 fps, i.e. 24 fps animation "on twos" (two identical
    // frames per drawing), the usual anime cadence. Zips from Pixiv /
    // Danbooru normally bundle animation.json and never use this.
    int ugoiraFrameDelayMs = 83;
    // One-click AI-content filter (omnibar AI toggle): hides posts tagged
    // ai_generated/ai_created/ai_assisted/ai_art everywhere.
    bool hideAi = false;
    // Captcha/PoW wall handling: pop the solve browser open by default
    // instead of just offering the wall menu (opt out in Preferences).
    bool wallAutoPopup = true;
    // Favourite tags (starred via sidebar/viewer right-click): they rank
    // first in search suggestions and sit behind the omnibar star menu.
    QStringList favTags;
    // Automatic tag-category lookup for untyped tags (cached forever).
    // Off by default for privacy: a fresh profile sends nothing. Backend
    // keys: off | hydrus | danbooru | gelbooru | safebooru | yandere |
    // custom. "hydrus" reads namespaced tags from hydrus.db (no network).
    QString classifyBackend = QStringLiteral("off");
    bool classifyAuto = false;
    QString classifyCustomUrl; // custom backend base URL
    QString classifyCustomFormat = QStringLiteral("danbooru"); // danbooru|gelbooru
    // Manual tag -> bucket overrides (tag type overrides in Preferences).
    // Lowercase tags, bucketForType() names; win over fetched verdicts,
    // applied without any network use.
    QMap<QString, QString> manualTagTypes;
    // Native custom category -> classic bucket (Nuuru flags, Szurubooru
    // categories, …): shown folded into that sidebar section and pill
    // colour instead of a standalone custom section. Lowercase names.
    QMap<QString, QString> customBucketMap;
    // Per-booru category merge/rename rules (Preferences → Taxonomies): group
    // native categories under one classic bucket or custom label.
    QList<TagCategoryMerge> tagCategoryMerges;
    // User-created categories and manual tag assignments (lowercase tag ->
    // category label). Assigned tags move out of their classic bucket in
    // the sidebar/viewer.
    QStringList customTagCategories;
    QMap<QString, QString> tagCustomCategories;
    // Legacy per-engine bucket folding (kept for old configs; the UI now
    // uses tagCategoryMerges instead).
    QSet<QString> disabledBuckets;
    // Tag classifier credentials live on the configured Gelbooru entry
    // (Preferences → Booru manager); anonymous otherwise.
    // Default source for new browse tabs (Ctrl+T / welcome Browse):
    // "browse" (online boorus), "local" (defaultLocalRoot) or "hydrus".
    QString defaultSource = QStringLiteral("browse");
    QString defaultLocalRoot;
    // Browse mode (remote boorus).
    QString browseMode = QStringLiteral("local"); // "local" | "browse"
    // New users start SFW-only: just Safebooru until they pick more in
    // Preferences → Browse → Default source.
    QStringList browseSites = {QStringLiteral("safebooru")};
    int browseLimit = 40;
    QString dataDir; // empty = default data dir (all DBs + downloads live here)
    QString downloadDir; // empty = <datadir>/downloads
    // Optional override for the config folder (settings.ini lives here).
    // Empty = default (~/.config/box). Applied on next launch.
    QString configDir;
    // Global proxy for all app traffic (http:// or socks5:// host:port).
    // SOCKS5 resolves hostnames through the proxy (curl socks5h
    // behavior), never locally. Empty = direct; a per-booru proxy
    // overrides this one; "direct" forces that booru direct.
    QString globalProxy;
    // Open tabs for session restore (local roots, browse sessions,
    // favourites). Saved on every tab change; capped when writing.
    struct SavedTab {
        QString kind; // "local" | "browse" | "favs"
        QString root;
        QStringList sites;
        QStringList tags;
        QString query;
        QString rating = QStringLiteral("all");
        QString category;
        QString sort = QStringLiteral("id-desc");
        bool hydrusOnlyMissing = false;
        QString viewerKey; // open post identity; empty = grid
        int scrollRow = -1; // first visible grid row
        bool operator==(const SavedTab &) const = default;
    };
    QList<SavedTab> savedTabs;
    int savedTabCurrent = 0;
    // Last place (any tab kind, local archives included): the home page
    // "Back to last tab" button resumes it, even across restarts. Updated
    // on every tab switch; empty kind = nothing to resume yet.
    SavedTab lastPlace;
    bool hasLastPlace = false;
    // Configured boorus (LoliSnatcher-style): presets for the popular ones
    // plus user-added instances. First run seeds the presets, folding the
    // legacy per-site credentials into them (src/migrations/).
    QList<BooruSite> boorus;
    // Explicit-detection tags (one per line in Preferences). Defaults to the
    // built-in set; saved once the dialog runs.
    QStringList explicitTags = defaultExplicitTagList();
    // Appearance (KeePassXC-style: Fusion pinned + own palette by default)
    QString themeMode = QStringLiteral("builtin"); // "builtin" | "native"
    QString accent = QStringLiteral("#3a7ec2");
    QString stylesheet; // optional custom .qss on top, empty = none
    // External media handlers. Empty = system default. {file}/{url}
    // placeholders are substituted; otherwise the path/URL is appended.
    QString defaultImageViewer;
    QString defaultGifViewer;
    QString defaultVideoPlayer;
    QString defaultTextEditor; // JSON sidecars / "View metadata"
    bool loopGif = true;
    bool loopVideo = true;
    // Home mascot: empty path = shipped default (マニフェスト.jpg);
    // mascotHidden hides even the default (Preferences → Remove).
    QString mascotPath;
    bool mascotHidden = false;
    double mascotOpacity = 0.35; // mascot translucency (0.1-1.0)
    double mascotScale = 0.45;   // height fraction (0.1-0.65), 1.0 = max
                                 // height, 2.0 = full width
    int mascotMargin = 0;        // mascot right/bottom padding in px
    // Grid
    int aspectNum = 1;
    int aspectDen = 1;
    int gridBoxW = 184; // grid post width px (size slider, 96-384)
    // Snap the grid width so a whole number of cards fills the viewport
    // with no leftover right gap (manual resizing keeps gaps when off).
    bool gridSnap = true;
    // Brick/staggered grid rows (odd columns shifted down half a card).
    // Off by default.
    bool gridStaggered = false;
    // Grid rows per wheel notch (scroll-speed setting, 0.5-3.0).
    // Fractional ticks accumulate, so high-res wheels stay smooth.
    double gridScrollRows = 0.5;
    // Playback
    bool videoMuted = true;
    bool videoLoop = true;
    // Preserve audio pitch when the playback rate is not 1× (Qt 6.10+).
    bool videoPitch = true;
    int volume = 100; // 0..100 default output volume
    // Viewer
    int viewerMaxSide = 2560; // huge-image decode cap
    // Startup + window
    QString tabsDefault = QStringLiteral("welcome"); // welcome|last|<sitekey>
    QByteArray geometry;
    QByteArray sideSplitter; // sidebar section sizes
    // Sidebar scroll mode: false = per-category scrollbars (default),
    // true = one big scrollbar through everything.
    bool sidebarSingleScroll = false;
    // Quick "+" menu: also list the local folder browser (off by default).
    bool showLocalFolderInPlusMenu = false;
};

// Shipped home mascot (kept under its original filename).
inline QString defaultMascotPath() {
    return QStringLiteral(":/icons/マニフェスト.jpg");
}

// What the home page should show: custom pick → default → nothing.
inline QString effectiveMascotPath(const AppSettings &s) {
    if (s.mascotHidden)
        return {};
    return s.mascotPath.isEmpty() ? defaultMascotPath() : s.mascotPath;
}

// Single data dir (favourites store.db, tag verdicts, downloads):
// Default XDG data location; override with AppSettings::dataDir.
// Cache bookkeeping (cache.db) lives in the platform cache dir instead.
inline QString defaultDataDir() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) +
           QStringLiteral("/box");
}

inline QString appDataDir() { return defaultDataDir(); }

// Effective data dir: the custom folder when set, otherwise the default.
// Point it at folder X and the app keeps store.db, tag_types.db,
// cache.db and downloads/ inside X. Trailing slashes are trimmed so
// "X/" and "X" resolve identically (no doubled separators downstream).
inline QString effectiveDataDir(const AppSettings &s) {
    if (!s.dataDir.isEmpty()) {
        QString d = s.dataDir;
        while (d.endsWith(u'/') && d.size() > 1)
            d.chop(1);
        return d;
    }
    return defaultDataDir();
}

// Tag-category verdicts: ONE database shared by every tab kind and booru
// engine (tags are the same strings everywhere - fetching once is enough,
// and archived dumps borrow it for free).
inline QString tagTypesDbPath() {
    return defaultDataDir() + QStringLiteral("/tag_types.db");
}

inline QString effectiveTagTypesDbPath(const AppSettings &s) {
    return effectiveDataDir(s) + QStringLiteral("/tag_types.db");
}

// Download folder (explicit Download action only; viewing never writes
// here). Empty setting = downloads/ under the (effective) data dir.
inline QString effectiveDownloadDir(const AppSettings &s) {
    if (!s.downloadDir.isEmpty()) {
        QString d = s.downloadDir;
        while (d.endsWith(u'/') && d.size() > 1)
            d.chop(1);
        return d;
    }
    return effectiveDataDir(s) + QStringLiteral("/downloads");
}

// Display label for a local archive tab: the saved archive's name when the
// root is configured in Preferences, else the folder name. Saved archives
// are matched by canonical path (symlinks included).
inline QString archiveLabelForRoot(const AppSettings &s, const QString &root) {
    const QString canonRoot = QDir(root).canonicalPath();
    for (const AppSettings::SavedArchive &a : s.archives) {
        const QString canon = QDir(a.root).canonicalPath();
        if (!canon.isEmpty() && canon == canonRoot) {
            const QString name = a.name.trimmed();
            if (!name.isEmpty())
                return name;
        }
    }
    const QString dirName = QDir(root).dirName();
    return dirName.isEmpty() ? root : dirName;
}

// Hydrus tab media folder + database. The tab is isolated: these never
// point at downloads/ or store.db unless the user explicitly does so.
inline QString effectiveHydrusDir(const AppSettings &s) {
    if (!s.hydrusDir.trimmed().isEmpty()) {
        QString d = s.hydrusDir;
        while (d.endsWith(u'/') && d.size() > 1)
            d.chop(1);
        return d;
    }
    return effectiveDataDir(s) + QStringLiteral("/hydrus");
}

inline QString effectiveHydrusDbPath(const AppSettings &s) {
    if (!s.hydrusDb.trimmed().isEmpty()) {
        QString p = s.hydrusDb;
        while (p.endsWith(u'/') && p.size() > 1)
            p.chop(1);
        return p;
    }
    return effectiveDataDir(s) + QStringLiteral("/hydrus.db");
}

// Classifier backend keys are stored as plain strings so old configs and
// exports stay readable. Unknown values fall back to "off".
inline QString classifyBackendKey(const QString &raw) {
    const QString k = raw.trimmed().toLower();
    if (k == QStringLiteral("hydrus") || k == QStringLiteral("danbooru") ||
        k == QStringLiteral("gelbooru") || k == QStringLiteral("safebooru") ||
        k == QStringLiteral("yandere") || k == QStringLiteral("custom"))
        return k;
    return QStringLiteral("off");
}

inline bool classifyBackendIsOnline(const QString &key) {
    const QString k = classifyBackendKey(key);
    return k == QStringLiteral("danbooru") || k == QStringLiteral("gelbooru") ||
           k == QStringLiteral("safebooru") || k == QStringLiteral("yandere") ||
           k == QStringLiteral("custom");
}

inline bool classifyAutoEnabled(const AppSettings &s) {
    return s.classifyAuto && classifyBackendKey(s.classifyBackend) !=
                                 QStringLiteral("off");
}

inline bool classifyNeedsNetwork(const AppSettings &s) {
    return classifyAutoEnabled(s) &&
           classifyBackendIsOnline(s.classifyBackend);
}

// True when every persisted field matches (window geometry excluded:
// it changes on every move and must not keep Apply lit).
inline bool operator==(const AppSettings &a, const AppSettings &b) {
    if (a.boorus.size() != b.boorus.size() ||
        a.savedTabs.size() != b.savedTabs.size() ||
        a.archives.size() != b.archives.size())
        return false;
    for (int i = 0; i < a.archives.size(); ++i) {
        if (!(a.archives.at(i) == b.archives.at(i)))
            return false;
    }
    for (int i = 0; i < a.boorus.size(); ++i) {
        if (!(a.boorus.at(i) == b.boorus.at(i)))
            return false;
    }
    for (int i = 0; i < a.savedTabs.size(); ++i) {
        if (!(a.savedTabs.at(i) == b.savedTabs.at(i)))
            return false;
    }
    if (a.hasLastPlace != b.hasLastPlace)
        return false;
    if (a.hasLastPlace && !(a.lastPlace == b.lastPlace))
        return false;
    return a.indexCache == b.indexCache &&
           a.dedupAllowArchiveDelete == b.dedupAllowArchiveDelete &&
           a.cacheMode == b.cacheMode &&
           a.fullCache == b.fullCache && a.cacheLimitMB == b.cacheLimitMB &&
           a.cacheDir == b.cacheDir &&
           a.cacheFavs == b.cacheFavs &&
           a.thumbLongSide == b.thumbLongSide && a.lastRoot == b.lastRoot &&
           a.history == b.history && a.historyTokens == b.historyTokens &&
           a.restoreSession == b.restoreSession &&
           a.language == b.language &&
           a.favAutoDownload == b.favAutoDownload && a.favAutoFav == b.favAutoFav &&
           a.unfavDeletesFile == b.unfavDeletesFile &&
           a.deleteUnfavs == b.deleteUnfavs && a.strayAutoFav == b.strayAutoFav &&
           a.hydrusTagLookups == b.hydrusTagLookups &&
           a.hydrusTab == b.hydrusTab && a.hydrusUrl == b.hydrusUrl &&
           a.hydrusAccessKey == b.hydrusAccessKey &&
           a.hydrusProxy == b.hydrusProxy &&
           a.hydrusDir == b.hydrusDir && a.hydrusDb == b.hydrusDb &&
           a.hydrusMinTagPosts == b.hydrusMinTagPosts &&
           a.hydrusDropUnknownTags == b.hydrusDropUnknownTags &&
           a.shareBind == b.shareBind && a.sharePort == b.sharePort &&
           a.shareHydrus == b.shareHydrus &&
           a.shareDownloads == b.shareDownloads &&
           a.shareFavourites == b.shareFavourites &&
           a.shareLocalFolder == b.shareLocalFolder &&
           a.shareFolder == b.shareFolder &&
           a.dragLinkMode == b.dragLinkMode &&
           a.blacklist == b.blacklist &&
           a.hideAi == b.hideAi && a.wallAutoPopup == b.wallAutoPopup &&
           a.favTags == b.favTags &&
           a.ugoiraFormat == b.ugoiraFormat &&
           a.ugoiraFrameDelayMs == b.ugoiraFrameDelayMs &&
           a.classifyBackend == b.classifyBackend &&
           a.classifyAuto == b.classifyAuto &&
           a.classifyCustomUrl == b.classifyCustomUrl &&
           a.classifyCustomFormat == b.classifyCustomFormat &&
           a.manualTagTypes == b.manualTagTypes &&
           a.customBucketMap == b.customBucketMap &&
           a.tagCategoryMerges == b.tagCategoryMerges &&
           a.customTagCategories == b.customTagCategories &&
           a.tagCustomCategories == b.tagCustomCategories &&
           a.disabledBuckets == b.disabledBuckets &&
           a.defaultSource == b.defaultSource &&
           a.defaultLocalRoot == b.defaultLocalRoot &&
           a.browseMode == b.browseMode && a.browseSites == b.browseSites &&
           a.browseLimit == b.browseLimit && a.dataDir == b.dataDir &&
           a.downloadDir == b.downloadDir && a.globalProxy == b.globalProxy &&
           a.savedTabCurrent == b.savedTabCurrent &&
           a.explicitTags == b.explicitTags && a.themeMode == b.themeMode &&
           a.accent == b.accent && a.stylesheet == b.stylesheet &&
           a.defaultImageViewer == b.defaultImageViewer &&
           a.defaultGifViewer == b.defaultGifViewer &&
           a.defaultVideoPlayer == b.defaultVideoPlayer &&
           a.defaultTextEditor == b.defaultTextEditor &&
           a.loopGif == b.loopGif && a.loopVideo == b.loopVideo &&
           a.mascotPath == b.mascotPath && a.mascotHidden == b.mascotHidden &&
           qFuzzyCompare(a.mascotOpacity + 1.0, b.mascotOpacity + 1.0) &&
           qFuzzyCompare(a.mascotScale + 1.0, b.mascotScale + 1.0) &&
           a.mascotMargin == b.mascotMargin &&
           a.aspectNum == b.aspectNum && a.aspectDen == b.aspectDen &&
           a.gridBoxW == b.gridBoxW && a.gridSnap == b.gridSnap &&
           a.gridStaggered == b.gridStaggered &&
           a.gridScrollRows == b.gridScrollRows &&
           a.videoMuted == b.videoMuted &&
           a.videoLoop == b.videoLoop && a.videoPitch == b.videoPitch &&
           a.volume == b.volume &&
           a.viewerMaxSide == b.viewerMaxSide &&
           a.tabsDefault == b.tabsDefault && a.sideSplitter == b.sideSplitter &&
           a.sidebarSingleScroll == b.sidebarSingleScroll &&
           a.showLocalFolderInPlusMenu == b.showLocalFolderInPlusMenu;
}

inline bool operator!=(const AppSettings &a, const AppSettings &b) {
    return !(a == b);
}

// Single config file (~/.config/box/settings.ini).
QString settingsFilePath();
void setConfigDirOverride(const QString &dir);
QString effectiveConfigDir();
// Copy settings.ini + settings.ini.bak + boorus.json from `fromDir`
// to `toDir` (created if needed). Returns empty on success, error text
// otherwise. Does not write the stub; see writeConfigStub().
QString moveConfigFiles(const QString &fromDir, const QString &toDir);
// Write the pointer stub ([app] configDir) to the default config folder,
// so next launch bootstraps straight to `customDir`. Atomic (tmp+rename).
QString writeConfigStub(const QString &defaultDir, const QString &customDir);
// Record an opened archive root: most-recent-first, deduped, capped.
void noteFolderOpened(AppSettings &s, const QString &dir);
// Previous-good copy (settings.ini.bak): rotated on every atomic save,
// restored when the main file is missing or truncated by a crash.
QString settingsBackupFilePath();
// Copy the backup over the main file when the main file is missing or
// empty (crash mid-rotate). Called at the top of loadSettings().
void restoreSettingsBackupIfNeeded();
QString appCacheDir(); // QStandardPaths::CacheLocation (includes org/app names)
void setAppCacheDirOverride(const QString &dir);
AppSettings loadSettings();
void saveSettings(const AppSettings &s);

// Booru registry persistence (order + per-key groups). First call seeds
// presets and folds legacy per-site credentials (one-time upgrade in
// src/migrations/m_booru_credentials.cpp). Declared here so the
// preferences dialog and tests share the same load/save path.
class QSettings;
QList<BooruSite> loadBoorus(QSettings &c);
void saveBoorus(QSettings &c, const QList<BooruSite> &boorus);

// Wipes the scan-index cache (fresh rescan after). Media caches are left
// alone; use CacheJanitor::purgeMedia() for those.
void clearIndexCache();
