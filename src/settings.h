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

struct AppSettings {
    // Library
    bool indexCache = true;
    bool thumbCache = true;
    // Full-quality image cache while browsing. OFF by default: silent full
    // downloads would fill the disk with images you never asked to keep.
    bool fullCache = false;
    // Total cache cap in MB (thumbs + full images). 0 = unlimited.
    int cacheLimitMB = 10240;
    int thumbLongSide = 360; // 256/360/512/768
    QString lastRoot;
    QStringList history; // most-recent-first archive roots, cap 10
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
    // Blacklisted tags: hidden everywhere (local + browse).
    QStringList blacklist;
    // Online tag-category lookup for untyped tags (cached forever).
    bool classifyTags = false;
    // Manual tag -> bucket overrides (tag type overrides in Preferences).
    // Lowercase tags, bucketForType() names; win over fetched verdicts,
    // applied without any network use.
    QMap<QString, QString> manualTagTypes;
    // Disabled tag buckets per booru type ("gelbooru:character"): parsed
    // tags in those buckets fold into general. For meme hosts and other
    // non-anime taxonomies. "general" itself is always on.
    QSet<QString> disabledBuckets;
    // Tag classifier credentials live on the configured Gelbooru entry
    // (Preferences → Booru manager); anonymous otherwise.
    // Browse mode (remote boorus).
    QString browseMode = QStringLiteral("local"); // "local" | "browse"
    // New users start SFW-only: just Safebooru until they pick more in
    // Preferences → Browse → Default boorus.
    QStringList browseSites = {QStringLiteral("safebooru")};
    int browseLimit = 40;
    QString dataDir; // empty = default data dir (all DBs + downloads live here)
    QString downloadDir; // empty = <datadir>/downloads
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
    };
    QList<SavedTab> savedTabs;
    int savedTabCurrent = 0;
    // Last place (any tab kind, local archives included): the home page
    // "Back to last tab" button resumes it, even across restarts. Updated
    // on every tab switch; empty kind = nothing to resume yet.
    SavedTab lastPlace;
    bool hasLastPlace = false;
    // Configured boorus (LoliSnatcher-style): presets for the popular ones
    // plus user-added instances. First run seeds the presets and migrates
    // the legacy per-site credentials into them.
    QList<BooruSite> boorus;
    // Explicit-detection tags (one per line in Preferences). Defaults to the
    // built-in set; saved once the dialog runs.
    QStringList explicitTags = defaultExplicitTagList();
    // Appearance (KeePassXC-style: Fusion pinned + own palette by default)
    QString themeMode = QStringLiteral("builtin"); // "builtin" | "native"
    QString accent = QStringLiteral("#3a7ec2");
    QString stylesheet; // optional custom .qss on top, empty = none
    QString mascotPath; // optional home-page mascot image, empty = none
    double mascotOpacity = 0.9; // mascot translucency (0.1-1.0)
    double mascotScale = 0.45;   // height fraction (0.1-0.65), 1.0 = max
                                 // height, 2.0 = full width
    int mascotMargin = 0;        // mascot right/bottom padding in px
    // Grid
    int aspectNum = 1;
    int aspectDen = 1;
    int gridBoxW = 184; // grid post width px (size slider, 96-384)
    // Grid rows per wheel notch (scroll-speed setting, 0.5-3.0).
    // Fractional ticks accumulate, so high-res wheels stay smooth.
    double gridScrollRows = 0.5;
    // Playback
    bool videoMuted = true;
    bool videoLoop = true;
    int volume = 100; // 0..100 default output volume
    // Viewer
    int viewerMaxSide = 2560; // huge-image decode cap
    // Startup + window
    QString tabsDefault = QStringLiteral("welcome"); // welcome|last|<sitekey>
    QByteArray geometry;
    QByteArray sideSplitter; // sidebar section sizes
};

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
// engine (tags are the same strings everywhere — fetching once is enough,
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

inline bool operator==(const AppSettings::SavedTab &a,
                       const AppSettings::SavedTab &b) {
    return a.kind == b.kind && a.root == b.root && a.sites == b.sites &&
           a.tags == b.tags && a.query == b.query && a.rating == b.rating &&
           a.category == b.category && a.sort == b.sort;
}

inline bool booruSiteEqual(const BooruSite &a, const BooruSite &b) {
    return a.key == b.key && a.label == b.label && a.shortName == b.shortName &&
           a.badgeColor == b.badgeColor && a.baseUrl == b.baseUrl &&
           a.engine == b.engine && a.user == b.user &&
           a.secret == b.secret && a.cookies == b.cookies &&
           a.skipAutoClassify == b.skipAutoClassify &&
           a.disabledBuckets == b.disabledBuckets &&
           a.defaultTags == b.defaultTags && a.proxy == b.proxy;
}

// True when every persisted field matches (window geometry excluded:
// it changes on every move and must not keep Apply lit).
inline bool operator==(const AppSettings &a, const AppSettings &b) {
    if (a.boorus.size() != b.boorus.size() ||
        a.savedTabs.size() != b.savedTabs.size())
        return false;
    for (int i = 0; i < a.boorus.size(); ++i) {
        if (!booruSiteEqual(a.boorus.at(i), b.boorus.at(i)))
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
    return a.indexCache == b.indexCache && a.thumbCache == b.thumbCache &&
           a.fullCache == b.fullCache && a.cacheLimitMB == b.cacheLimitMB &&
           a.thumbLongSide == b.thumbLongSide && a.lastRoot == b.lastRoot &&
           a.history == b.history && a.restoreSession == b.restoreSession &&
           a.language == b.language &&
           a.favAutoDownload == b.favAutoDownload && a.favAutoFav == b.favAutoFav &&
           a.unfavDeletesFile == b.unfavDeletesFile &&
           a.deleteUnfavs == b.deleteUnfavs && a.strayAutoFav == b.strayAutoFav &&
           a.blacklist == b.blacklist &&
           a.classifyTags == b.classifyTags && a.manualTagTypes == b.manualTagTypes &&
           a.disabledBuckets == b.disabledBuckets &&
           a.browseMode == b.browseMode && a.browseSites == b.browseSites &&
           a.browseLimit == b.browseLimit && a.dataDir == b.dataDir &&
           a.downloadDir == b.downloadDir && a.globalProxy == b.globalProxy &&
           a.savedTabCurrent == b.savedTabCurrent &&
           a.explicitTags == b.explicitTags && a.themeMode == b.themeMode &&
           a.accent == b.accent && a.stylesheet == b.stylesheet &&
           a.mascotPath == b.mascotPath &&
           qFuzzyCompare(a.mascotOpacity + 1.0, b.mascotOpacity + 1.0) &&
           qFuzzyCompare(a.mascotScale + 1.0, b.mascotScale + 1.0) &&
           a.mascotMargin == b.mascotMargin &&
           a.aspectNum == b.aspectNum && a.aspectDen == b.aspectDen &&
           a.gridBoxW == b.gridBoxW && a.gridScrollRows == b.gridScrollRows &&
           a.videoMuted == b.videoMuted &&
           a.videoLoop == b.videoLoop && a.volume == b.volume &&
           a.viewerMaxSide == b.viewerMaxSide &&
           a.tabsDefault == b.tabsDefault && a.sideSplitter == b.sideSplitter;
}

inline bool operator!=(const AppSettings &a, const AppSettings &b) {
    return !(a == b);
}

// Single config file (~/.config/box/settings.ini).
QString settingsFilePath();
QString appCacheDir(); // QStandardPaths::CacheLocation (includes org/app names)
AppSettings loadSettings();
void saveSettings(const AppSettings &s);

// Booru registry persistence (order + per-key groups). First call seeds
// presets and migrates legacy per-site credentials. Declared here so the
// preferences dialog and tests share the same load/save path.
class QSettings;
QList<BooruSite> loadBoorus(QSettings &c);
void saveBoorus(QSettings &c, const QList<BooruSite> &boorus);

// Wipes the scan-index + thumbnail caches (fresh rescan/re-decode after).
// Only our own cache subdirs are touched.
void clearCaches();
