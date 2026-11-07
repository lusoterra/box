#include "settings.h"
#include "cachepolicy.h"
#include "tagtypes.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QStandardPaths>

#include <algorithm>

static QString s_configDirOverride;
void setConfigDirOverride(const QString &dir) { s_configDirOverride = dir; }
QString effectiveConfigDir() {
    if (!s_configDirOverride.isEmpty()) {
        QString d = s_configDirOverride;
        while (d.endsWith(u'/') && d.size() > 1) d.chop(1);
        return d;
    }
    return QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) +
           QStringLiteral("/box");
}
QString settingsFilePath() {
    return effectiveConfigDir() + QStringLiteral("/settings.ini");
}

QString settingsBackupFilePath() {
    return settingsFilePath() + QStringLiteral(".bak");
}

void restoreSettingsBackupIfNeeded() {
    const QString path = settingsFilePath();
    const QFileInfo info(path);
    if (info.exists() && info.size() > 0)
        return; // main file intact: nothing to heal
    if (!QFile::exists(settingsBackupFilePath()))
        return; // nothing to restore from
    if (info.exists())
        QFile::remove(path); // truncated/empty husk
    QFile::copy(settingsBackupFilePath(), path);
}

static QString s_cacheDirOverride;
void setAppCacheDirOverride(const QString &dir) { s_cacheDirOverride = dir; }
QString appCacheDir() {
    if (!s_cacheDirOverride.isEmpty())
        return s_cacheDirOverride;
    return QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
}

namespace {
QSettings openSettings() {
    return QSettings(settingsFilePath(), QSettings::IniFormat);
}
QString dragModeKey(DragLinkMode m) {
    switch (m) {
    case DragLinkMode::DirectMedia:
        return QStringLiteral("direct");
    case DragLinkMode::PostPage:
        return QStringLiteral("post");
    case DragLinkMode::Inherit:
        break;
    }
    return QStringLiteral("inherit");
}
DragLinkMode dragModeFromKey(const QString &key, DragLinkMode def) {
    if (key == QStringLiteral("direct"))
        return DragLinkMode::DirectMedia;
    if (key == QStringLiteral("post"))
        return DragLinkMode::PostPage;
    if (key == QStringLiteral("inherit"))
        return DragLinkMode::Inherit;
    return def;
}
} // namespace

AppSettings loadSettings() {
    restoreSettingsBackupIfNeeded();
    AppSettings s;
    QSettings c = openSettings();
    s.indexCache = c.value(QStringLiteral("cache/index"), s.indexCache).toBool();
    if (c.contains(QStringLiteral("cache/mode"))) {
        s.cacheMode = MediaCache::modeFromKey(
            c.value(QStringLiteral("cache/mode")).toString(), s.cacheMode);
    } else if (!c.value(QStringLiteral("cache/thumbs"), true).toBool()) {
        // Legacy "thumbnail disk cache" toggle: off meant no media cache.
        s.cacheMode = CacheMode::None;
    }
    if (s.cacheMode == CacheMode::Inherit)
        s.cacheMode = CacheMode::All;
    s.fullCache = c.value(QStringLiteral("cache/full"), s.fullCache).toBool();
    s.searchHistory =
        c.value(QStringLiteral("search/history"), s.searchHistory).toBool();
    s.dragLinkMode = dragModeFromKey(
        c.value(QStringLiteral("drag/linkMode")).toString(), s.dragLinkMode);
    if (s.dragLinkMode == DragLinkMode::Inherit)
        s.dragLinkMode = DragLinkMode::PostPage;
    s.cacheLimitMB =
        qBound(0, c.value(QStringLiteral("cache/limitMB"), s.cacheLimitMB).toInt(),
               1048576);
    s.cacheDir = c.value(QStringLiteral("cache/dir"), s.cacheDir).toString();
    s.cacheFavs = c.value(QStringLiteral("cache/favs"), s.cacheFavs).toBool();
    s.thumbLongSide = qBound(
        128, c.value(QStringLiteral("cache/thumbSize"), s.thumbLongSide).toInt(),
        1024);
    s.lastRoot = c.value(QStringLiteral("library/root")).toString();
    s.history = c.value(QStringLiteral("library/history")).toStringList();
    s.historyTokens =
        c.value(QStringLiteral("library/historyTokens")).toStringList().mid(0, 200);
    // NOTE: never a group literally named general/General - QSettings
    // reserves General for groupless keys, so that group can neither be
    // written readably nor read back (settings silently reset every
    // launch). app/ holds these instead; General/ is a read fallback
    // for files written before the rename.
    s.restoreSession = c.value(QStringLiteral("app/restoreSession"),
                               c.value(QStringLiteral("General/restoreSession"),
                                       s.restoreSession)).toBool();
    s.language = c.value(QStringLiteral("app/language"),
                         c.value(QStringLiteral("General/language"),
                                 s.language)).toString();
    s.favAutoDownload =
        c.value(QStringLiteral("fav/autoDownload"), s.favAutoDownload).toBool();
    s.favAutoFav = c.value(QStringLiteral("fav/autoFav"), s.favAutoFav).toBool();
    s.unfavDeletesFile =
        c.value(QStringLiteral("fav/unfavDeletesFile"), s.unfavDeletesFile).toBool();
    s.deleteUnfavs =
        c.value(QStringLiteral("fav/deleteUnfavs"), s.deleteUnfavs).toBool();
    s.strayAutoFav =
        c.value(QStringLiteral("stray/autoFav"), s.strayAutoFav).toBool();
    if (c.contains(QStringLiteral("filter/blacklist")))
        s.blacklist =
            normalizeExplicitTags(c.value(QStringLiteral("filter/blacklist")).toStringList());
    s.hideAi = c.value(QStringLiteral("filter/hideAi"), s.hideAi).toBool();
    s.wallAutoPopup = c.value(QStringLiteral("filter/wallAutoPopup"), s.wallAutoPopup).toBool();
    s.ugoiraFormat =
        c.value(QStringLiteral("downloads/ugoiraFormat"), s.ugoiraFormat).toString();
    if (s.ugoiraFormat != QStringLiteral("webp") &&
        s.ugoiraFormat != QStringLiteral("jxl") &&
        s.ugoiraFormat != QStringLiteral("zip"))
        s.ugoiraFormat = QStringLiteral("webp");
    s.ugoiraFrameDelayMs =
        qBound(20, c.value(QStringLiteral("downloads/ugoiraDelayMs"),
                           s.ugoiraFrameDelayMs).toInt(),
               2000);
    if (c.contains(QStringLiteral("fav/tags")))
        s.favTags = c.value(QStringLiteral("fav/tags")).toStringList();
    s.classifyTags =
        c.value(QStringLiteral("classify/enabled"), s.classifyTags).toBool();
    if (c.contains(QStringLiteral("tagtypes/manual")))
        s.manualTagTypes = TagTypes::normalizeTagOverrides(
            c.value(QStringLiteral("tagtypes/manual")).toStringList());
    if (c.contains(QStringLiteral("tagtypes/customBuckets")))
        s.customBucketMap = TagTypes::normalizeTagOverrides(
            c.value(QStringLiteral("tagtypes/customBuckets")).toStringList());
    s.tagCategoryMerges = TagCategories::fromLines(
        c.value(QStringLiteral("tagtypes/categoryMerges")).toStringList());
    s.customTagCategories.clear();
    for (const QString &cat :
         c.value(QStringLiteral("tagtypes/customCategories")).toStringList()) {
        const QString t = cat.trimmed();
        if (!t.isEmpty() && !s.customTagCategories.contains(t))
            s.customTagCategories << t;
    }
    s.tagCustomCategories.clear();
    for (const QString &line :
         c.value(QStringLiteral("tagtypes/tagCustomCategories"))
             .toStringList()) {
        const int eq = line.indexOf(u'=');
        if (eq <= 0)
            continue;
        const QString tag = line.left(eq).trimmed().toLower();
        const QString cat = line.mid(eq + 1).trimmed();
        if (!tag.isEmpty() && !cat.isEmpty())
            s.tagCustomCategories.insert(tag, cat);
    }
    if (c.contains(QStringLiteral("tagtypes/disabledBuckets"))) {
        s.disabledBuckets.clear();
        for (const QString &e :
             c.value(QStringLiteral("tagtypes/disabledBuckets")).toStringList()) {
            const QString t = e.trimmed().toLower();
            if (!t.isEmpty())
                s.disabledBuckets.insert(t);
        }
    }
    s.browseMode = c.value(QStringLiteral("browse/mode"), s.browseMode).toString();
    if (c.contains(QStringLiteral("browse/sites")))
        s.browseSites = c.value(QStringLiteral("browse/sites")).toStringList();
    s.browseLimit = qBound(10, c.value(QStringLiteral("browse/limit"), s.browseLimit).toInt(), 200);
    s.savedTabs.clear();
    c.beginGroup(QStringLiteral("tabs/saved"));
    QList<int> order;
    for (const QString &g : c.childGroups()) {
        bool numOk = false;
        const int idx = g.toInt(&numOk);
        if (numOk)
            order << idx;
    }
    std::sort(order.begin(), order.end());
    for (int idx : order) {
        c.beginGroup(QString::number(idx));
        AppSettings::SavedTab t;
        t.kind = c.value(QStringLiteral("kind")).toString();
        t.root = c.value(QStringLiteral("root")).toString();
        t.sites = c.value(QStringLiteral("sites")).toStringList();
        t.tags = c.value(QStringLiteral("tags")).toStringList();
        t.query = c.value(QStringLiteral("query")).toString();
        t.rating = c.value(QStringLiteral("rating"), QStringLiteral("all")).toString();
        t.category = c.value(QStringLiteral("category")).toString();
        t.sort = c.value(QStringLiteral("sort"), QStringLiteral("id-desc")).toString();
        t.viewerKey = c.value(QStringLiteral("viewerKey")).toString();
        t.scrollRow = c.value(QStringLiteral("scrollRow"), -1).toInt();
        c.endGroup();
        if (!t.kind.isEmpty())
            s.savedTabs.append(t);
        if (s.savedTabs.size() >= 25)
            break;
    }
    c.endGroup();
    s.savedTabCurrent =
        c.value(QStringLiteral("tabs/savedCurrent"), 0).toInt();
    // Last place for the home-page resume button (any tab kind).
    s.hasLastPlace = false;
    if (!c.value(QStringLiteral("last/kind")).toString().isEmpty()) {
        s.hasLastPlace = true;
        s.lastPlace.kind = c.value(QStringLiteral("last/kind")).toString();
        s.lastPlace.root = c.value(QStringLiteral("last/root")).toString();
        s.lastPlace.sites = c.value(QStringLiteral("last/sites")).toStringList();
        s.lastPlace.tags = c.value(QStringLiteral("last/tags")).toStringList();
        s.lastPlace.query = c.value(QStringLiteral("last/query")).toString();
        s.lastPlace.rating =
            c.value(QStringLiteral("last/rating"), QStringLiteral("all")).toString();
        s.lastPlace.category = c.value(QStringLiteral("last/category")).toString();
        s.lastPlace.sort =
            c.value(QStringLiteral("last/sort"), QStringLiteral("id-desc")).toString();
        s.lastPlace.viewerKey = c.value(QStringLiteral("last/viewerKey")).toString();
        s.lastPlace.scrollRow = c.value(QStringLiteral("last/scrollRow"), -1).toInt();
    }
    s.downloadDir = c.value(QStringLiteral("downloads/dir")).toString();
    s.globalProxy = c.value(QStringLiteral("network/proxy")).toString();
    s.dataDir = c.value(QStringLiteral("storage/dataDir")).toString();
    s.tabsDefault = c.value(QStringLiteral("startup/defaultTab"), s.tabsDefault).toString();
    if (!c.contains(QStringLiteral("startup/defaultTab")) && s.restoreSession)
        s.tabsDefault = QStringLiteral("last"); // legacy toggle
    s.geometry = c.value(QStringLiteral("window/geometry")).toByteArray();
    s.sideSplitter = c.value(QStringLiteral("window/sideSplitter")).toByteArray();
    s.sidebarSingleScroll =
        c.value(QStringLiteral("window/sidebarSingleScroll"), false).toBool();
    s.showLocalFolderInPlusMenu =
        c.value(QStringLiteral("window/showLocalFolderInPlusMenu"), false)
            .toBool();
    s.boorus = loadBoorus(c);
    if (c.contains(QStringLiteral("explicit/tags")))
        s.explicitTags =
            normalizeExplicitTags(c.value(QStringLiteral("explicit/tags")).toStringList());
    s.themeMode = c.value(QStringLiteral("theme/mode"), s.themeMode).toString();
    s.accent = c.value(QStringLiteral("theme/accent"), s.accent).toString();
    s.stylesheet = c.value(QStringLiteral("theme/stylesheet")).toString();
    s.defaultImageViewer =
        c.value(QStringLiteral("app/defaultImageViewer")).toString();
    s.defaultVideoPlayer =
        c.value(QStringLiteral("app/defaultVideoPlayer")).toString();
    s.mascotPath = c.value(QStringLiteral("home/mascot")).toString();
    s.mascotHidden =
        c.value(QStringLiteral("home/mascotHidden"), false).toBool();
    s.mascotOpacity = qBound(
        0.1, c.value(QStringLiteral("home/mascotOpacity"), s.mascotOpacity).toDouble(),
        1.0);
    s.mascotScale = qBound(
        0.1, c.value(QStringLiteral("home/mascotScale"), s.mascotScale).toDouble(),
        2.0);
    s.mascotMargin =
        qBound(0, c.value(QStringLiteral("home/mascotMargin"), s.mascotMargin).toInt(),
               256);
    s.aspectNum = c.value(QStringLiteral("grid/aspectNum"), s.aspectNum).toInt();
    s.aspectDen = c.value(QStringLiteral("grid/aspectDen"), s.aspectDen).toInt();
    s.gridBoxW =
        qBound(96, c.value(QStringLiteral("grid/boxW"), s.gridBoxW).toInt(), 384);
    s.gridSnap =
        c.value(QStringLiteral("grid/snap"), s.gridSnap).toBool();
    s.gridStaggered =
        c.value(QStringLiteral("grid/staggered"), s.gridStaggered).toBool();
    s.gridScrollRows = qBound(
        0.1, c.value(QStringLiteral("grid/scrollRows"), s.gridScrollRows).toDouble(),
        3.0);
    if (s.aspectNum < 1 || s.aspectDen < 1) {
        s.aspectNum = 1;
        s.aspectDen = 1;
    }
    s.videoMuted = c.value(QStringLiteral("viewer/muted"), s.videoMuted).toBool();
    s.videoLoop = c.value(QStringLiteral("viewer/loop"), s.videoLoop).toBool();
    s.volume = qBound(0, c.value(QStringLiteral("viewer/volume"), s.volume).toInt(), 100);
    s.viewerMaxSide = qBound(
        512, c.value(QStringLiteral("viewer/maxSide"), s.viewerMaxSide).toInt(),
        8192);
    return s;
}

namespace {
// Full ini content; target file supplied by the caller so saveSettings()
// can stage through a temp file (atomic replace, see below).
void writeSettingsTo(QSettings &c, const AppSettings &s) {
    c.setValue(QStringLiteral("cache/index"), s.indexCache);
    c.setValue(QStringLiteral("cache/mode"), MediaCache::modeKey(s.cacheMode));
    // Legacy key kept in sync for older builds reading the same ini.
    c.setValue(QStringLiteral("cache/thumbs"),
               s.cacheMode != CacheMode::None);
    c.setValue(QStringLiteral("cache/full"), s.fullCache);
    c.setValue(QStringLiteral("drag/linkMode"), dragModeKey(s.dragLinkMode));
    c.setValue(QStringLiteral("search/history"), s.searchHistory);
    c.setValue(QStringLiteral("cache/limitMB"), s.cacheLimitMB);
    c.setValue(QStringLiteral("cache/dir"), s.cacheDir);
    c.setValue(QStringLiteral("cache/favs"), s.cacheFavs);
    c.setValue(QStringLiteral("cache/thumbSize"), s.thumbLongSide);
    c.setValue(QStringLiteral("library/root"), s.lastRoot);
    c.setValue(QStringLiteral("library/history"), s.history);
    c.setValue(QStringLiteral("library/historyTokens"),
               s.historyTokens.mid(0, 200));
    c.setValue(QStringLiteral("app/restoreSession"), s.restoreSession);
    c.setValue(QStringLiteral("app/language"), s.language);
    c.setValue(QStringLiteral("fav/autoDownload"), s.favAutoDownload);
    c.setValue(QStringLiteral("fav/autoFav"), s.favAutoFav);
    c.setValue(QStringLiteral("fav/unfavDeletesFile"), s.unfavDeletesFile);
    c.setValue(QStringLiteral("fav/deleteUnfavs"), s.deleteUnfavs);
    c.setValue(QStringLiteral("stray/autoFav"), s.strayAutoFav);
    c.setValue(QStringLiteral("filter/blacklist"), s.blacklist);
    c.setValue(QStringLiteral("filter/hideAi"), s.hideAi);
    c.setValue(QStringLiteral("filter/wallAutoPopup"), s.wallAutoPopup);
    c.setValue(QStringLiteral("downloads/ugoiraFormat"), s.ugoiraFormat);
    c.setValue(QStringLiteral("downloads/ugoiraDelayMs"), s.ugoiraFrameDelayMs);
    c.setValue(QStringLiteral("fav/tags"), s.favTags);
    c.setValue(QStringLiteral("classify/enabled"), s.classifyTags);
    c.setValue(QStringLiteral("tagtypes/manual"),
                 TagTypes::serializeTagOverrides(s.manualTagTypes));
    c.setValue(QStringLiteral("tagtypes/customBuckets"),
                 TagTypes::serializeTagOverrides(s.customBucketMap));
    c.setValue(QStringLiteral("tagtypes/categoryMerges"),
               TagCategories::toLines(s.tagCategoryMerges));
    c.setValue(QStringLiteral("tagtypes/customCategories"),
               s.customTagCategories);
    QStringList customPairs;
    for (auto it = s.tagCustomCategories.begin();
         it != s.tagCustomCategories.end(); ++it)
        customPairs << it.key() + u'=' + it.value();
    c.setValue(QStringLiteral("tagtypes/tagCustomCategories"), customPairs);
    c.setValue(QStringLiteral("tagtypes/disabledBuckets"),
               QStringList(s.disabledBuckets.begin(), s.disabledBuckets.end()));
    c.setValue(QStringLiteral("browse/mode"), s.browseMode);
    c.setValue(QStringLiteral("browse/sites"), s.browseSites);
    c.setValue(QStringLiteral("browse/limit"), s.browseLimit);
    c.remove(QStringLiteral("tabs/saved"));
    c.beginGroup(QStringLiteral("tabs/saved"));
    int n = 0;
    for (const AppSettings::SavedTab &t : s.savedTabs) {
        if (n >= 25)
            break; // bound ini growth
        c.beginGroup(QString::number(n++));
        c.setValue(QStringLiteral("kind"), t.kind);
        c.setValue(QStringLiteral("root"), t.root);
        c.setValue(QStringLiteral("sites"), t.sites);
        c.setValue(QStringLiteral("tags"), t.tags);
        c.setValue(QStringLiteral("query"), t.query);
        c.setValue(QStringLiteral("rating"), t.rating);
        c.setValue(QStringLiteral("category"), t.category);
        c.setValue(QStringLiteral("sort"), t.sort);
        c.setValue(QStringLiteral("viewerKey"), t.viewerKey);
        c.setValue(QStringLiteral("scrollRow"), t.scrollRow);
        c.endGroup();
    }
    c.endGroup();
    c.setValue(QStringLiteral("tabs/savedCurrent"), s.savedTabCurrent);
    c.remove(QStringLiteral("last"));
    if (s.hasLastPlace && !s.lastPlace.kind.isEmpty()) {
        c.setValue(QStringLiteral("last/kind"), s.lastPlace.kind);
        c.setValue(QStringLiteral("last/root"), s.lastPlace.root);
        c.setValue(QStringLiteral("last/sites"), s.lastPlace.sites);
        c.setValue(QStringLiteral("last/tags"), s.lastPlace.tags);
        c.setValue(QStringLiteral("last/query"), s.lastPlace.query);
        c.setValue(QStringLiteral("last/rating"), s.lastPlace.rating);
        c.setValue(QStringLiteral("last/category"), s.lastPlace.category);
        c.setValue(QStringLiteral("last/sort"), s.lastPlace.sort);
        c.setValue(QStringLiteral("last/viewerKey"), s.lastPlace.viewerKey);
        c.setValue(QStringLiteral("last/scrollRow"), s.lastPlace.scrollRow);
    }
    c.setValue(QStringLiteral("downloads/dir"), s.downloadDir);
    c.setValue(QStringLiteral("network/proxy"), s.globalProxy);
    c.setValue(QStringLiteral("storage/dataDir"), s.dataDir);
    c.setValue(QStringLiteral("startup/defaultTab"), s.tabsDefault);
    c.setValue(QStringLiteral("window/geometry"), s.geometry);
    c.setValue(QStringLiteral("window/sideSplitter"), s.sideSplitter);
    c.setValue(QStringLiteral("window/sidebarSingleScroll"),
               s.sidebarSingleScroll);
    c.setValue(QStringLiteral("window/showLocalFolderInPlusMenu"),
               s.showLocalFolderInPlusMenu);
    saveBoorus(c, s.boorus);
    c.setValue(QStringLiteral("explicit/tags"), s.explicitTags);
    c.setValue(QStringLiteral("theme/mode"), s.themeMode);
    c.setValue(QStringLiteral("theme/accent"), s.accent);
    c.setValue(QStringLiteral("theme/stylesheet"), s.stylesheet);
    c.setValue(QStringLiteral("app/defaultImageViewer"), s.defaultImageViewer);
    c.setValue(QStringLiteral("app/defaultVideoPlayer"), s.defaultVideoPlayer);
    c.setValue(QStringLiteral("home/mascot"), s.mascotPath);
    c.setValue(QStringLiteral("home/mascotHidden"), s.mascotHidden);
    c.setValue(QStringLiteral("home/mascotOpacity"), s.mascotOpacity);
    c.setValue(QStringLiteral("home/mascotScale"), s.mascotScale);
    c.setValue(QStringLiteral("home/mascotMargin"), s.mascotMargin);
    c.setValue(QStringLiteral("grid/aspectNum"), s.aspectNum);
    c.setValue(QStringLiteral("grid/aspectDen"), s.aspectDen);
    c.setValue(QStringLiteral("grid/boxW"), s.gridBoxW);
    c.setValue(QStringLiteral("grid/snap"), s.gridSnap);
    c.setValue(QStringLiteral("grid/staggered"), s.gridStaggered);
    c.setValue(QStringLiteral("grid/scrollRows"), s.gridScrollRows);
    c.setValue(QStringLiteral("viewer/muted"), s.videoMuted);
    c.setValue(QStringLiteral("viewer/loop"), s.videoLoop);
    c.setValue(QStringLiteral("viewer/volume"), s.volume);
    c.setValue(QStringLiteral("viewer/maxSide"), s.viewerMaxSide);
    c.remove(QStringLiteral("privacy/paranoia")); // retired: false promise
    c.sync();
}
} // namespace

void saveSettings(const AppSettings &s) {
    // Atomic replace + last-good backup: stage the full ini to a temp
    // file, then rotate (current -> .bak, temp -> current). The renames
    // are atomic, so a crash can never leave a truncated settings.ini;
    // at worst the main file is missing and loadSettings() heals it
    // from the backup.
    const QString path = settingsFilePath();
    const QString tmp = path + QStringLiteral(".tmp");
    const QString bak = settingsBackupFilePath();
    QFile::remove(tmp);
    {
        QSettings c(tmp, QSettings::IniFormat);
        writeSettingsTo(c, s);
        c.sync();
    }
    if (!QFile::exists(tmp))
        return; // staging failed: leave the current file alone
    QFile::remove(bak);
    if (QFile::exists(path))
        QFile::rename(path, bak);
    QFile::rename(tmp, path);
}

void noteFolderOpened(AppSettings &s, const QString &dir) {
    const QString d = dir.trimmed();
    if (d.isEmpty())
        return;
    s.history.removeAll(d);
    s.history.prepend(d);
    while (s.history.size() > 10)
        s.history.removeLast();
}

void clearIndexCache() {
    QDir(appCacheDir() + QStringLiteral("/roots")).removeRecursively();
}

namespace {
QString booruGroupKey(const QString &key) {
    QString k = key.trimmed();
    k.replace(u'/', u'_');
    return k.isEmpty() ? QStringLiteral("booru") : k;
}
} // namespace

// Configured boorus: order + one group per key. First run seeds the
// presets and folds the legacy per-site credentials/bases into them.
QList<BooruSite> loadBoorus(QSettings &c) {
    // Portable JSON wins when present.
    const QString jsonPath = QFileInfo(settingsFilePath()).absolutePath() +
                            QStringLiteral("/boorus.json");
    if (QFile::exists(jsonPath)) {
        QFile f(jsonPath);
        if (f.open(QIODevice::ReadOnly)) {
            const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
            if (doc.isArray()) {
                QList<BooruSite> out;
                for (const QJsonValue &v : doc.array()) {
                    const QJsonObject o = v.toObject();
                    BooruSite b;
                    b.key = o.value(QStringLiteral("key")).toString();
                    if (b.key.isEmpty())
                        continue;
                    b.label = o.value(QStringLiteral("label")).toString(b.key);
                    b.enabled = o.value(QStringLiteral("enabled")).toBool(true);
                    b.shortName = o.value(QStringLiteral("shortName")).toString();
                    if (b.shortName.isEmpty())
                        b.shortName = RemotePosts::shortFor(b.label);
                    b.badgeColor =
                        o.value(QStringLiteral("badgeColor")).toString(
                            QStringLiteral("#3a7ec2"));
                    b.baseUrl = o.value(QStringLiteral("baseUrl")).toString();
                    b.engine = RemotePosts::engineForKey(
                        o.value(QStringLiteral("engine")).toString(
                            QStringLiteral("danbooru")));
                    b.user = o.value(QStringLiteral("user")).toString();
                    b.secret = o.value(QStringLiteral("secret")).toString();
                    const QJsonValue cookies = o.value(QStringLiteral("cookies"));
                    if (cookies.isArray()) {
                        for (const QJsonValue &cv : cookies.toArray())
                            b.cookies << cv.toString();
                    } else if (cookies.isString() && !cookies.toString().isEmpty())
                        b.cookies << cookies.toString();
                    b.skipAutoClassify =
                        o.value(QStringLiteral("skipAutoClassify")).toBool(false);
                    const QJsonValue db = o.value(QStringLiteral("disabledBuckets"));
                    if (db.isArray())
                        for (const QJsonValue &dv : db.toArray())
                            b.disabledBuckets.insert(dv.toString());
                    b.defaultTags = o.value(QStringLiteral("defaultTags")).toString();
                    b.proxy = o.value(QStringLiteral("proxy")).toString();
                    b.localBlacklist =
                        o.value(QStringLiteral("localBlacklist")).toBool(false);
                    b.cacheMode = MediaCache::modeFromKey(
                        o.value(QStringLiteral("cacheMode")).toString(),
                        CacheMode::Inherit);
                    b.dragLinkMode = dragModeFromKey(
                        o.value(QStringLiteral("dragMode")).toString(),
                        DragLinkMode::Inherit);
                    out.append(b);
                }
                if (!out.isEmpty())
                    return out;
            }
        }
    }
    QList<BooruSite> out;
    const QStringList order = c.value(QStringLiteral("boorus/order")).toStringList();
    for (const QString &key : order) {
        if (key.trimmed().isEmpty())
            continue;
        c.beginGroup(QStringLiteral("boorus/") + booruGroupKey(key));
        BooruSite b;
        b.key = key;
        b.label = c.value(QStringLiteral("label"), key).toString();
        b.enabled = c.value(QStringLiteral("enabled"), true).toBool();
        b.shortName = c.value(QStringLiteral("short")).toString();
        if (b.shortName.isEmpty())
            b.shortName = RemotePosts::shortFor(b.label);
        b.badgeColor =
            c.value(QStringLiteral("badgeColor"), QStringLiteral("#3a7ec2"))
                .toString();
        b.baseUrl = c.value(QStringLiteral("base")).toString();
        b.engine = RemotePosts::engineForKey(
            c.value(QStringLiteral("engine"), QStringLiteral("danbooru")).toString());
        b.user = c.value(QStringLiteral("user")).toString();
        b.secret = c.value(QStringLiteral("secret")).toString();
        b.cookies = RemotePosts::normalizeCookies(
            c.value(QStringLiteral("cookies")).toStringList());
        b.skipAutoClassify =
            c.value(QStringLiteral("skipAutoClassify"), false).toBool();
        b.disabledBuckets.clear();
        for (const QString &bucket : c.value(QStringLiteral("disabledBuckets"))
                                         .toStringList()) {
            const QString t = bucket.trimmed().toLower();
            if (t == QStringLiteral("artist") || t == QStringLiteral("character") ||
                t == QStringLiteral("copyright") || t == QStringLiteral("meta"))
                b.disabledBuckets.insert(t);
        }
        b.defaultTags = c.value(QStringLiteral("tags")).toString();
        b.proxy = c.value(QStringLiteral("proxy")).toString();
        b.localBlacklist =
            c.value(QStringLiteral("localBlacklist"), false).toBool();
        b.cacheMode = MediaCache::modeFromKey(
            c.value(QStringLiteral("cacheMode")).toString(), CacheMode::Inherit);
        b.dragLinkMode = dragModeFromKey(
            c.value(QStringLiteral("dragMode")).toString(),
            DragLinkMode::Inherit);
        c.endGroup();
        out.append(b);
    }
    if (!out.isEmpty())
        return out;
    // Seed presets, migrating legacy credentials/bases onto them.
    out = RemoteSource::presetBoorus();
    auto migrateCreds = [&](const QString &key, const QString &userKey,
                            const QString &secretKey) {
        const QString user = c.value(userKey).toString();
        const QString secret = c.value(secretKey).toString();
        for (BooruSite &b : out) {
            if (b.key == key) {
                b.user = user;
                b.secret = secret;
            }
        }
    };
    migrateCreds(QStringLiteral("danbooru"), QStringLiteral("danbooru/user"),
                 QStringLiteral("danbooru/key"));
    migrateCreds(QStringLiteral("gelbooru"), QStringLiteral("gelbooru/user"),
                 QStringLiteral("gelbooru/key"));
    migrateCreds(QStringLiteral("yandere"), QStringLiteral("yandere/user"),
                 QStringLiteral("yandere/pass"));
    migrateCreds(QStringLiteral("konachan"), QStringLiteral("konachan/user"),
                 QStringLiteral("konachan/pass"));
    migrateCreds(QStringLiteral("szurubooru"), QStringLiteral("szurubooru/user"),
                 QStringLiteral("szurubooru/token"));
    for (BooruSite &b : out) {
        if (b.key == QStringLiteral("szurubooru"))
            b.baseUrl = c.value(QStringLiteral("szurubooru/base")).toString();
        else if (b.key == QStringLiteral("shimmie"))
            b.baseUrl = c.value(QStringLiteral("shimmie/base")).toString();
    }
    for (const QString &legacy :
         {QStringLiteral("danbooru/user"), QStringLiteral("danbooru/key"),
          QStringLiteral("gelbooru/user"), QStringLiteral("gelbooru/key"),
          QStringLiteral("yandere/user"), QStringLiteral("yandere/pass"),
          QStringLiteral("konachan/user"), QStringLiteral("konachan/pass"),
          QStringLiteral("szurubooru/base"), QStringLiteral("szurubooru/user"),
          QStringLiteral("szurubooru/token"), QStringLiteral("shimmie/base")})
        c.remove(legacy);
    saveBoorus(c, out);
    return out;
}

void saveBoorus(QSettings &c, const QList<BooruSite> &boorus) {
    // Drop groups for removed boorus so deletes persist.
    const QStringList old =
        c.value(QStringLiteral("boorus/order")).toStringList();
    QSet<QString> keep;
    for (const BooruSite &b : boorus)
        keep.insert(b.key);
    for (const QString &key : old) {
        if (!keep.contains(key))
            c.remove(QStringLiteral("boorus/") + booruGroupKey(key));
    }
    QStringList order;
    for (const BooruSite &b : boorus) {
        if (b.key.trimmed().isEmpty())
            continue;
        order << b.key;
        c.beginGroup(QStringLiteral("boorus/") + booruGroupKey(b.key));
        c.setValue(QStringLiteral("label"), b.label);
        c.setValue(QStringLiteral("enabled"), b.enabled);
        c.setValue(QStringLiteral("short"), b.shortName);
        c.setValue(QStringLiteral("badgeColor"), b.badgeColor);
        c.setValue(QStringLiteral("base"), b.baseUrl);
        c.setValue(QStringLiteral("engine"), RemotePosts::engineKey(b.engine));
        c.setValue(QStringLiteral("user"), b.user);
        c.setValue(QStringLiteral("secret"), b.secret);
        c.setValue(QStringLiteral("cookies"), b.cookies);
        c.setValue(QStringLiteral("skipAutoClassify"), b.skipAutoClassify);
        c.setValue(QStringLiteral("disabledBuckets"),
                   QStringList(b.disabledBuckets.begin(), b.disabledBuckets.end()));
        c.setValue(QStringLiteral("tags"), b.defaultTags);
        c.setValue(QStringLiteral("proxy"), b.proxy);
        c.setValue(QStringLiteral("localBlacklist"), b.localBlacklist);
        c.setValue(QStringLiteral("cacheMode"),
                   MediaCache::modeKey(b.cacheMode));
        c.setValue(QStringLiteral("dragMode"), dragModeKey(b.dragLinkMode));
        c.endGroup();
    }
    c.setValue(QStringLiteral("boorus/order"), order);
    // Portable JSON copy.
    QJsonArray arr;
    for (const BooruSite &b : boorus) {
        if (b.key.trimmed().isEmpty())
            continue;
        QJsonObject o;
        o[QStringLiteral("key")] = b.key;
        o[QStringLiteral("label")] = b.label;
        o[QStringLiteral("enabled")] = b.enabled;
        o[QStringLiteral("shortName")] = b.shortName;
        o[QStringLiteral("badgeColor")] = b.badgeColor;
        o[QStringLiteral("baseUrl")] = b.baseUrl;
        o[QStringLiteral("engine")] = RemotePosts::engineKey(b.engine);
        o[QStringLiteral("user")] = b.user;
        o[QStringLiteral("secret")] = b.secret;
        QJsonArray cookies;
        for (const QString &ck : b.cookies)
            cookies.append(ck);
        o[QStringLiteral("cookies")] = cookies;
        o[QStringLiteral("skipAutoClassify")] = b.skipAutoClassify;
        QJsonArray db;
        for (const QString &d : b.disabledBuckets)
            db.append(d);
        o[QStringLiteral("disabledBuckets")] = db;
        o[QStringLiteral("defaultTags")] = b.defaultTags;
        o[QStringLiteral("proxy")] = b.proxy;
        o[QStringLiteral("localBlacklist")] = b.localBlacklist;
        o[QStringLiteral("cacheMode")] = MediaCache::modeKey(b.cacheMode);
        o[QStringLiteral("dragMode")] = dragModeKey(b.dragLinkMode);
        arr.append(o);
    }
    QFile jf(QFileInfo(settingsFilePath()).absolutePath() +
             QStringLiteral("/boorus.json"));
    if (jf.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        jf.write(QJsonDocument(arr).toJson(QJsonDocument::Indented));
        jf.close();
    }
}
