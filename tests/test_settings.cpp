// Settings persistence: defaults, round-trip, effective dirs, legacy
// booru-credential migration. Fully isolated via
// XDG_CONFIG_HOME/XDG_DATA_HOME temp dirs (never touches real config).
#include "settings.h"
#include "cachepolicy.h"
#include "tagtypes.h"

#include <QTemporaryDir>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QtTest>

class TestSettings : public QObject {
    Q_OBJECT
    QTemporaryDir m_tmp;
private slots:
    void initTestCase() {
        QVERIFY(m_tmp.isValid());
        qputenv("XDG_CONFIG_HOME", m_tmp.filePath(QStringLiteral("config")).toUtf8());
        qputenv("XDG_DATA_HOME", m_tmp.filePath(QStringLiteral("data")).toUtf8());
    }

    void defaultsSane() {
        const AppSettings s;
        QVERIFY(s.indexCache);
        QVERIFY(s.cacheMode == CacheMode::All); // thumbnails cached like before
        QVERIFY(!s.hideAi); // AI-content filter defaults OFF
        QVERIFY(s.wallAutoPopup); // wall solve browser auto-pops
        QVERIFY(s.customBucketMap.isEmpty());
        QVERIFY(!s.fullCache); // silent full downloads default OFF
        QCOMPARE(s.cacheLimitMB, 10240);
        QVERIFY(s.videoMuted && s.videoLoop);
        QCOMPARE(s.ugoiraFormat, QStringLiteral("webp"));
        QCOMPARE(s.ugoiraFrameDelayMs, 100);
        QCOMPARE(s.explicitTags, defaultExplicitTagList());
        QVERIFY(s.downloadDir.isEmpty());
        QVERIFY(s.dataDir.isEmpty());
        QCOMPARE(s.browseSites, QStringList({QStringLiteral("safebooru")}));
        QCOMPARE(s.tabsDefault, QStringLiteral("welcome"));
        QVERIFY(!s.favAutoDownload); // download-on-favourite defaults OFF
        QVERIFY(s.favAutoFav);       // favourite-on-download defaults ON
        QVERIFY(s.unfavDeletesFile); // delete-on-unfavourite defaults ON
        QVERIFY(!s.deleteUnfavs);    // unfavourite-on-delete defaults OFF
        QVERIFY(!s.hasLastPlace);    // nothing to resume on first run
    }

    void settingsRoundTrip() {
        AppSettings s;
        s.indexCache = false;
        s.cacheMode = CacheMode::SfwOnly;
        s.cacheLimitMB = 512;
        s.thumbLongSide = 512;
        s.lastRoot = QStringLiteral("/pics/dump");
        s.history = {QStringLiteral("/pics/a"), QStringLiteral("/pics/b")};
        s.historyTokens = {QStringLiteral("sky"), QStringLiteral("cloud")};
        s.downloadDir = QStringLiteral("/tmp/be-dl");
        s.dataDir = QStringLiteral("/tmp/be-data");
        s.globalProxy = QStringLiteral("socks5h://127.0.0.1:9050");
        s.favAutoDownload = true;
        s.favAutoFav = true;
        s.unfavDeletesFile = true;
        s.deleteUnfavs = true;
        s.hideAi = true;
        s.wallAutoPopup = false;
        s.customBucketMap.insert(QStringLiteral("series"), QStringLiteral("copyright"));
        s.tagCategoryMerges.append(
            {QStringLiteral("soy"), {QStringLiteral("*")},
             QStringLiteral("soybooru_specific")});
        s.customTagCategories = {QStringLiteral("Favourite faces")};
        s.tagCustomCategories.insert(QStringLiteral("smile"),
                                     QStringLiteral("Favourite faces"));
        s.blacklist = {QStringLiteral("loli"), QStringLiteral("shota")};
        s.browseSites = {QStringLiteral("danbooru")};
        BooruSite custom;
        custom.key = QStringLiteral("mybooru");
        custom.label = QStringLiteral("My Booru");
        custom.shortName = QStringLiteral("MYB");
        custom.badgeColor = QStringLiteral("#a1b2c3");
        custom.baseUrl = QStringLiteral("https://booru.example");
        custom.engine = BooruEngine::Moebooru;
        custom.user = QStringLiteral("user");
        custom.secret = QStringLiteral("key");
        custom.cookies = {QStringLiteral("cf_clearance=abc"),
                          QStringLiteral("sess=xyz")};
        custom.skipAutoClassify = true;
        custom.disabledBuckets = {QStringLiteral("character")};
        custom.defaultTags = QStringLiteral("rating:safe solo");
        custom.proxy = QStringLiteral("socks5://127.0.0.1:1080");
        custom.localBlacklist = true;
        custom.enabled = false;
        custom.cacheMode = CacheMode::None;
        s.boorus = {custom};
        s.explicitTags = {QStringLiteral("customtag")};
        s.disabledBuckets = {QStringLiteral("gelbooru:character")};
        s.volume = 42;
        s.ugoiraFormat = QStringLiteral("jxl");
        s.ugoiraFrameDelayMs = 150;
        s.aspectNum = 4;
        s.aspectDen = 3;
        s.gridBoxW = 256;
        s.gridScrollRows = 2.5;
        s.tabsDefault = QStringLiteral("last");
        s.language = QStringLiteral("fr");
        s.mascotPath = QStringLiteral("/pics/mascot.png");
        s.mascotHidden = true;
        s.mascotOpacity = 0.5;
        s.mascotScale = 0.25;
        s.mascotMargin = 12;
        s.hasLastPlace = true;
        s.lastPlace.kind = QStringLiteral("browse");
        s.lastPlace.sites = {QStringLiteral("danbooru")};
        s.lastPlace.tags = {QStringLiteral("sky")};
        s.lastPlace.sort = QStringLiteral("score-desc");
        saveSettings(s);
        const AppSettings back = loadSettings();
        QCOMPARE(back.indexCache, false);
        QVERIFY(back.cacheMode == CacheMode::SfwOnly);
        QCOMPARE(back.cacheLimitMB, 512);
        QCOMPARE(back.thumbLongSide, 512);
        QCOMPARE(back.lastRoot, QStringLiteral("/pics/dump"));
        QCOMPARE(back.history, s.history);
        QCOMPARE(back.historyTokens, s.historyTokens);
        QCOMPARE(back.downloadDir, QStringLiteral("/tmp/be-dl"));
        QCOMPARE(back.dataDir, QStringLiteral("/tmp/be-data"));
        QCOMPARE(back.globalProxy,
                 QStringLiteral("socks5h://127.0.0.1:9050"));
        QVERIFY(back.favAutoDownload);
        QVERIFY(back.favAutoFav);
        QVERIFY(back.unfavDeletesFile);
        QVERIFY(back.deleteUnfavs);
        QVERIFY(back.hideAi);
        QVERIFY(!back.wallAutoPopup);
        QCOMPARE(back.customBucketMap.value(QStringLiteral("series")), QStringLiteral("copyright"));
        QCOMPARE(back.tagCategoryMerges, s.tagCategoryMerges);
        QCOMPARE(back.customTagCategories, s.customTagCategories);
        QCOMPARE(back.tagCustomCategories, s.tagCustomCategories);
        QCOMPARE(back.blacklist, s.blacklist);
        QCOMPARE(back.browseSites, QStringList({QStringLiteral("danbooru")}));
        QCOMPARE(back.boorus.size(), 1);
        QCOMPARE(back.boorus.first().key, QStringLiteral("mybooru"));
        QCOMPARE(back.boorus.first().label, QStringLiteral("My Booru"));
        QCOMPARE(back.boorus.first().baseUrl, QStringLiteral("https://booru.example"));
        QCOMPARE(back.boorus.first().engine, BooruEngine::Moebooru);
        QCOMPARE(back.boorus.first().user, QStringLiteral("user"));
        QCOMPARE(back.boorus.first().secret, QStringLiteral("key"));
        QCOMPARE(back.boorus.first().shortName, QStringLiteral("MYB"));
        QCOMPARE(back.boorus.first().badgeColor, QStringLiteral("#a1b2c3"));
        QCOMPARE(back.boorus.first().cookies, custom.cookies);
        QVERIFY(back.boorus.first().skipAutoClassify);
        QCOMPARE(back.boorus.first().disabledBuckets,
                 QSet<QString>({QStringLiteral("character")}));
        QCOMPARE(back.boorus.first().defaultTags,
                 QStringLiteral("rating:safe solo"));
        QCOMPARE(back.boorus.first().proxy,
                 QStringLiteral("socks5://127.0.0.1:1080"));
        QVERIFY(back.boorus.first().localBlacklist);
        QVERIFY(!back.boorus.first().enabled);
        QVERIFY(back.boorus.first().cacheMode == CacheMode::None);
        QCOMPARE(back.disabledBuckets,
                 QSet<QString>({QStringLiteral("gelbooru:character")}));
        QCOMPARE(back.explicitTags, QStringList({QStringLiteral("customtag")}));
        QCOMPARE(back.volume, 42);
        QCOMPARE(back.ugoiraFormat, QStringLiteral("jxl"));
        QCOMPARE(back.ugoiraFrameDelayMs, 150);
        QCOMPARE(back.aspectNum, 4);
        QCOMPARE(back.aspectDen, 3);
        QCOMPARE(back.gridBoxW, 256);
        QCOMPARE(back.gridScrollRows, 2.5);
        QCOMPARE(back.tabsDefault, QStringLiteral("last"));
        QCOMPARE(back.language, QStringLiteral("fr"));
        QCOMPARE(back.mascotPath, QStringLiteral("/pics/mascot.png"));
        QVERIFY(back.mascotHidden);
        QCOMPARE(back.mascotOpacity, 0.5);
        QCOMPARE(back.mascotScale, 0.25);
        QCOMPARE(back.mascotMargin, 12);
        QVERIFY(back.hasLastPlace);
        QCOMPARE(back.lastPlace.kind, QStringLiteral("browse"));
        QCOMPARE(back.lastPlace.sites, QStringList({QStringLiteral("danbooru")}));
        QCOMPARE(back.lastPlace.tags, QStringList({QStringLiteral("sky")}));
        QCOMPARE(back.lastPlace.sort, QStringLiteral("score-desc"));
    }

    void listsNormalizeOnLoad() {
        AppSettings s = loadSettings();
        s.blacklist = {QStringLiteral("  Foo "), QStringLiteral("foo"), QStringLiteral("BAR")};
        s.explicitTags = {QStringLiteral(" X "), QStringLiteral("x")};
        saveSettings(s);
        const AppSettings back = loadSettings();
        QCOMPARE(back.blacklist, QStringList({QStringLiteral("bar"), QStringLiteral("foo")}));
        QCOMPARE(back.explicitTags, QStringList({QStringLiteral("x")}));
    }

    void folderHistoryStacks() {
        AppSettings s;
        noteFolderOpened(s, QString());
        QVERIFY(s.history.isEmpty());
        noteFolderOpened(s, QStringLiteral("/pics/b"));
        noteFolderOpened(s, QStringLiteral("/pics/a"));
        noteFolderOpened(s, QStringLiteral("/pics/a")); // reopen = first
        QCOMPARE(s.history,
                 QStringList({QStringLiteral("/pics/a"),
                              QStringLiteral("/pics/b")}));
        for (int i = 0; i < 12; ++i)
            noteFolderOpened(s, QStringLiteral("/pics/%1").arg(i));
        QCOMPARE(s.history.size(), 10);
        QCOMPARE(s.history.first(), QStringLiteral("/pics/11"));
    }

    void manualTagTypesRoundTrip() {
        AppSettings s = loadSettings();
        s.manualTagTypes.insert(QStringLiteral("lucky_star"),
                                QStringLiteral("copyright"));
        s.manualTagTypes.insert(QStringLiteral("hatsune_miku"),
                                QStringLiteral("character"));
        saveSettings(s);
        QCOMPARE(loadSettings().manualTagTypes, s.manualTagTypes);
        // Load normalizes raw ini lines (case, spacing, garbage dropped).
        AppSettings raw = loadSettings();
        raw.manualTagTypes = TagTypes::normalizeTagOverrides(
            {QStringLiteral(" Lucky_Star = Copyright "),
             QStringLiteral("bogus line"), QStringLiteral("x=nope")});
        saveSettings(raw);
        const QMap<QString, QString> back = loadSettings().manualTagTypes;
        QCOMPARE(back.size(), 1);
        QCOMPARE(back.value(QStringLiteral("lucky_star")),
                 QStringLiteral("copyright"));
    }

    void favTagsRoundTrip() {
        AppSettings s = loadSettings();
        s.favTags = {QStringLiteral("hatsune_miku"),
                     QStringLiteral("1girl")};
        saveSettings(s);
        QCOMPARE(loadSettings().favTags, s.favTags);
        s.favTags.clear();
        saveSettings(s);
        QVERIFY(loadSettings().favTags.isEmpty());
    }

    void cachePolicyNarrowsPerBooru() {
        BooruSite noCache;
        noCache.key = QStringLiteral("nocache");
        noCache.cacheMode = CacheMode::None;
        BooruSite sfw;
        sfw.key = QStringLiteral("sfw");
        sfw.cacheMode = CacheMode::SfwOnly;
        const QList<BooruSite> sites = {noCache, sfw};
        MediaCache::configure(CacheMode::All, sites);
        QVERIFY(MediaCache::persistFor(QStringLiteral("other"),
                                       QStringLiteral("explicit")));
        QVERIFY(!MediaCache::persistFor(QStringLiteral("nocache"),
                                        QStringLiteral("general")));
        QVERIFY(MediaCache::persistFor(QStringLiteral("sfw"),
                                       QStringLiteral("general")));
        QVERIFY(!MediaCache::persistFor(QStringLiteral("sfw"),
                                        QStringLiteral("explicit")));
        // Global "nothing" is a hard floor: per-site "all" cannot reopen.
        MediaCache::configure(CacheMode::None, {});
        QVERIFY(!MediaCache::persistFor(QStringLiteral("other"),
                                        QStringLiteral("general")));
        MediaCache::configure(CacheMode::All, {});
    }

    void tagCategoryMergeRules() {
        TagCategoryMerge rename;
        rename.siteKey = QStringLiteral("soy");
        rename.sources = {QStringLiteral("character")};
        rename.target = QStringLiteral("Characters");
        TagCategoryMerge custom;
        custom.sources = {QStringLiteral("*")};
        custom.target = QStringLiteral("soybooru_specific");
        const QList<TagCategoryMerge> rules = {rename, custom};
        QCOMPARE(TagCategories::classicBucket(QStringLiteral("Characters")),
                 QStringLiteral("character"));
        QVERIFY(TagCategories::mergeFor(rules, QStringLiteral("soy"),
                                        QStringLiteral("character")) == &rules[0]);
        QVERIFY(TagCategories::mergeFor(rules, QStringLiteral("gelbooru"),
                                        QStringLiteral("character")) == &rules[1]);
        QCOMPARE(TagCategories::labelFor(rules, QStringLiteral("soy"),
                                         QStringLiteral("character")),
                 QStringLiteral("Characters"));
        QCOMPARE(TagCategories::targetKey(custom.target),
                 QStringLiteral("soybooru specific"));
        QCOMPARE(TagCategories::displayTarget(custom.target),
                 QStringLiteral("Soybooru specific"));
        const QList<TagCategoryMerge> back = TagCategories::fromLines(
            TagCategories::toLines(rules));
        QCOMPARE(back, rules);

        // Gap coverage (src/library/tagcategories.h)
        QCOMPARE(TagCategories::classicBucket(QStringLiteral(" ip ")),
                 QStringLiteral("copyright"));
        QCOMPARE(TagCategories::classicBucket(QStringLiteral("metadata")),
                 QStringLiteral("meta"));
        QCOMPARE(TagCategories::classicBucket(QStringLiteral("Uncategorized")),
                 QStringLiteral("general"));
        QCOMPARE(TagCategories::classicBucket(QStringLiteral("mystery")),
                 QString());
        TagCategoryMerge m;
        m.siteKey = QStringLiteral("Soy");
        m.sources = {QStringLiteral("*")};
        m.target = QStringLiteral("  my_group  ");
        QVERIFY(TagCategories::scopeMatches(m, QStringLiteral("soy")));
        QVERIFY(!TagCategories::scopeMatches(m, QStringLiteral("dan")));
        m.siteKey.clear();
        QVERIFY(TagCategories::scopeMatches(m, QStringLiteral("any")));
        QCOMPARE(TagCategories::targetKey(QStringLiteral("Characters")),
                 QStringLiteral("character"));
        TagCategoryMerge empty;
        empty.sources = {QStringLiteral("x")};
        QVERIFY(TagCategories::toLines({empty}).isEmpty());
        const QList<TagCategoryMerge> parsed = TagCategories::fromLines(
            {QStringLiteral("soy|character,variant|Characters"),
             QStringLiteral("badline"),
             QStringLiteral("||"),
             QStringLiteral("all||Target")});
        QCOMPARE(parsed.size(), 1);
        QCOMPARE(parsed[0].siteKey, QStringLiteral("soy"));
        QCOMPARE(parsed[0].sources,
                 QStringList({QStringLiteral("character"),
                              QStringLiteral("variant")}));
    }

    void downloadedNsfwBypassesSfwCacheOnly() {
        BooruSite sfw;
        sfw.key = QStringLiteral("sfw");
        sfw.cacheMode = CacheMode::SfwOnly;
        MediaCache::configure(CacheMode::SfwOnly, {sfw});
        Post p;
        p.isRemote = true;
        p.siteKey = QStringLiteral("sfw");
        p.rating = QStringLiteral("explicit");
        // Favourited only: still denied while SFW-only.
        QVERIFY(!MediaCache::persistForPost(p));
        // Explicitly downloaded bytes lift the rating gate.
        p.downloaded = true;
        QVERIFY(MediaCache::persistForPost(p));
        // "Nothing" stays a hard floor, downloads included.
        MediaCache::configure(CacheMode::None, {sfw});
        QVERIFY(!MediaCache::persistForPost(p));
        MediaCache::configure(CacheMode::All, {});
    }

    void sfwCacheIsAggressiveAndHonorsBlacklist() {
        BooruSite sfw;
        sfw.key = QStringLiteral("sfw");
        sfw.cacheMode = CacheMode::SfwOnly;
        MediaCache::configure(CacheMode::SfwOnly, {sfw});
        MediaCache::setSfwBlacklist({QStringLiteral("guro")});
        Post p;
        p.isRemote = true;
        p.siteKey = QStringLiteral("sfw");
        p.rating.clear(); // unknown is not provably SFW
        QVERIFY(!MediaCache::persistForPost(p));
        p.tags = QStringLiteral("1girl rating:g");
        QVERIFY(MediaCache::persistForPost(p)); // explicit rating tag
        p.tags = QStringLiteral("1girl guro rating:g");
        QVERIFY(!MediaCache::persistForPost(p)); // blacklist wins
        p.downloaded = true;
        QVERIFY(!MediaCache::persistForPost(p)); // blacklist beats downloads
        MediaCache::setSfwBlacklist({});
        p.tags = QStringLiteral("1girl rating:g");
        QVERIFY(MediaCache::persistForPost(p));
        p.rating = QStringLiteral("general");
        QVERIFY(MediaCache::persistForPost(p));
        p.rating = QStringLiteral("explicit");
        QVERIFY(MediaCache::persistForPost(p)); // downloaded exception
        p.downloaded = false;
        QVERIFY(!MediaCache::persistForPost(p));
        MediaCache::setSfwBlacklist({});
        MediaCache::configure(CacheMode::All, {});
    }

    void legacyCredsMigrateToBoorus() {        // Simulate a pre-registry config: legacy keys, no boorus/order.
        // Fresh profiles seed Safebooru-only now; legacy credentials for
        // unshipped boorus are dropped (existing profiles keep theirs).
        QSettings c(settingsFilePath(), QSettings::IniFormat);
        c.clear();
        c.setValue(QStringLiteral("danbooru/user"), QStringLiteral("duser"));
        c.setValue(QStringLiteral("danbooru/key"), QStringLiteral("dkey"));
        c.setValue(QStringLiteral("gelbooru/user"), QStringLiteral("guser"));
        c.setValue(QStringLiteral("gelbooru/key"), QStringLiteral("gkey"));
        c.setValue(QStringLiteral("szurubooru/base"), QStringLiteral("https://szu.local"));
        c.setValue(QStringLiteral("szurubooru/token"), QStringLiteral("tok"));
        c.sync();
        const AppSettings back = loadSettings();
        QCOMPARE(back.boorus.size(), 1);
        QCOMPARE(back.boorus.first().key, QStringLiteral("safebooru"));
        QCOMPARE(back.boorus.first().label, QStringLiteral("Safebooru"));
        QCOMPARE(back.boorus.first().baseUrl,
                 QStringLiteral("https://safebooru.donmai.us"));
        QCOMPARE(back.boorus.first().engine, BooruEngine::Danbooru);
        // Second load keeps the seeded list (no reseed, no wipe).
        QCOMPARE(loadSettings().boorus.size(), 1);
    }

    void savedTabsRoundTrip() {
        AppSettings s = loadSettings();
        AppSettings::SavedTab local;
        local.kind = QStringLiteral("local");
        local.root = QStringLiteral("/pics/dump");
        local.tags = {QStringLiteral("1girl")};
        local.sort = QStringLiteral("score-desc");
        AppSettings::SavedTab browse;
        browse.kind = QStringLiteral("browse");
        browse.sites = {QStringLiteral("danbooru"), QStringLiteral("gelbooru")};
        browse.tags = {QStringLiteral("sky")};
        browse.query = QStringLiteral("sky");
        browse.rating = QStringLiteral("general");
        browse.viewerKey = QStringLiteral("danbooru#42");
        browse.scrollRow = 17;
        AppSettings::SavedTab favs;
        favs.kind = QStringLiteral("favs");
        s.savedTabs = {local, browse, favs};
        s.savedTabCurrent = 2;
        saveSettings(s);
        const AppSettings back = loadSettings();
        QCOMPARE(back.savedTabs.size(), 3);
        QCOMPARE(back.savedTabs.at(0).kind, QStringLiteral("local"));
        QCOMPARE(back.savedTabs.at(0).root, QStringLiteral("/pics/dump"));
        QCOMPARE(back.savedTabs.at(0).tags, QStringList({QStringLiteral("1girl")}));
        QCOMPARE(back.savedTabs.at(0).sort, QStringLiteral("score-desc"));
        QCOMPARE(back.savedTabs.at(1).kind, QStringLiteral("browse"));
        QCOMPARE(back.savedTabs.at(1).sites,
                 QStringList({QStringLiteral("danbooru"), QStringLiteral("gelbooru")}));
        QCOMPARE(back.savedTabs.at(1).query, QStringLiteral("sky"));
        QCOMPARE(back.savedTabs.at(1).viewerKey, QStringLiteral("danbooru#42"));
        QCOMPARE(back.savedTabs.at(1).scrollRow, 17);
        QCOMPARE(back.savedTabs.at(2).kind, QStringLiteral("favs"));
        QCOMPARE(back.savedTabCurrent, 2);
    }

    void atomicSaveRotatesBackup() {
        // Two saves: the main file holds the newest tabs, the .bak the
        // previous (last-good) ones. Proves the tmp+rename rotation.
        AppSettings s = loadSettings();
        AppSettings::SavedTab first;
        first.kind = QStringLiteral("browse");
        first.sites = {QStringLiteral("danbooru")};
        s.savedTabs = {first};
        s.savedTabCurrent = 0;
        saveSettings(s);
        AppSettings::SavedTab second;
        second.kind = QStringLiteral("favs");
        s.savedTabs = {second};
        saveSettings(s);
        QVERIFY(QFile::exists(settingsFilePath()));
        QVERIFY(QFile::exists(settingsBackupFilePath()));
        QCOMPARE(loadSettings().savedTabs.size(), 1);
        QCOMPARE(loadSettings().savedTabs.first().kind, QStringLiteral("favs"));
        QSettings bak(settingsBackupFilePath(), QSettings::IniFormat);
        bak.beginGroup(QStringLiteral("tabs/saved/0"));
        QCOMPARE(bak.value(QStringLiteral("kind")).toString(),
                 QStringLiteral("browse"));
        QCOMPARE(bak.value(QStringLiteral("sites")).toStringList(),
                 QStringList({QStringLiteral("danbooru")}));
    }

    void emptyMainRestoresFromBackup() {
        // A crash that leaves a truncated settings.ini heals from the
        // backup on the next load (tabs survive).
        AppSettings s = loadSettings();
        AppSettings::SavedTab favs;
        favs.kind = QStringLiteral("favs");
        favs.sort = QStringLiteral("added");
        s.savedTabs = {favs};
        s.savedTabCurrent = 0;
        saveSettings(s); // also refreshes the .bak chain
        saveSettings(s); // .bak now holds the favs tab too
        QFile main(settingsFilePath());
        QVERIFY(main.open(QIODevice::WriteOnly | QIODevice::Truncate));
        main.close();
        QCOMPARE(QFileInfo(settingsFilePath()).size(), 0);
        const AppSettings back = loadSettings();
        QCOMPARE(back.savedTabs.size(), 1);
        QCOMPARE(back.savedTabs.first().kind, QStringLiteral("favs"));
    }

    void effectiveDirs() {
        AppSettings s;
        QCOMPARE(effectiveDataDir(s), appDataDir());
        QCOMPARE(effectiveDownloadDir(s), appDataDir() + QStringLiteral("/downloads"));
        QCOMPARE(effectiveTagTypesDbPath(s),
                 appDataDir() + QStringLiteral("/tag_types.db"));
        s.downloadDir = QStringLiteral("/custom/dl");
        QCOMPARE(effectiveDownloadDir(s), QStringLiteral("/custom/dl"));
        // Pointing the data dir at folder X keeps everything inside X.
        s.downloadDir.clear();
        s.dataDir = QStringLiteral("/custom/X");
        QCOMPARE(effectiveDataDir(s), QStringLiteral("/custom/X"));
        QCOMPARE(effectiveDownloadDir(s), QStringLiteral("/custom/X/downloads"));
        QCOMPARE(effectiveTagTypesDbPath(s),
                 QStringLiteral("/custom/X/tag_types.db"));
        // An explicit download dir still wins over the data dir.
        s.downloadDir = QStringLiteral("/custom/dl");
        QCOMPARE(effectiveDownloadDir(s), QStringLiteral("/custom/dl"));
        // Trailing slashes never double separators downstream.
        s.dataDir = QStringLiteral("/custom/X/");
        s.downloadDir.clear();
        QCOMPARE(effectiveDataDir(s), QStringLiteral("/custom/X"));
        QCOMPARE(effectiveDownloadDir(s),
                 QStringLiteral("/custom/X/downloads"));
        s.downloadDir = QStringLiteral("/custom/dl/");
        QCOMPARE(effectiveDownloadDir(s), QStringLiteral("/custom/dl"));
    }

    void tagCategoryMergeGaps() {
        // src/library/tagcategories.h
        QCOMPARE(TagCategories::normalizeKey(QStringLiteral("")), QString());
        QCOMPARE(TagCategories::normalizeKey(QStringLiteral("  ")), QString());
        QCOMPARE(TagCategories::normalizeKey(QStringLiteral("A_B-C")), QStringLiteral("a b c"));
        QCOMPARE(TagCategories::classicBucket(QStringLiteral("artists")), QStringLiteral("artist"));
        QCOMPARE(TagCategories::classicBucket(QStringLiteral("creators")), QStringLiteral("artist"));
        QCOMPARE(TagCategories::classicBucket(QStringLiteral("characters")), QStringLiteral("character"));
        QCOMPARE(TagCategories::classicBucket(QStringLiteral("chars")), QStringLiteral("character"));
        QCOMPARE(TagCategories::classicBucket(QStringLiteral("subvariants")), QStringLiteral("character"));
        QCOMPARE(TagCategories::classicBucket(QStringLiteral("copyrights")), QStringLiteral("copyright"));
        QCOMPARE(TagCategories::classicBucket(QStringLiteral("franchise")), QStringLiteral("copyright"));
        QCOMPARE(TagCategories::classicBucket(QStringLiteral("franchises")), QStringLiteral("copyright"));
        QCOMPARE(TagCategories::classicBucket(QStringLiteral("metas")), QStringLiteral("meta"));
        QCOMPARE(TagCategories::classicBucket(QStringLiteral("medium")), QStringLiteral("meta"));
        QCOMPARE(TagCategories::classicBucket(QStringLiteral("media")), QStringLiteral("meta"));
        QCOMPARE(TagCategories::classicBucket(QStringLiteral("title")), QStringLiteral("meta"));
        QCOMPARE(TagCategories::classicBucket(QStringLiteral("titles")), QStringLiteral("meta"));
        QCOMPARE(TagCategories::classicBucket(QStringLiteral("general")), QStringLiteral("general"));
        QCOMPARE(TagCategories::classicBucket(QStringLiteral("bogus")), QString());
    }
};

int main(int argc, char *argv[]) {
    // QCoreApplication (not appless): QSettings path handling and friends
    // require an application instance. Still no display needed.
    QCoreApplication app(argc, argv);
    TestSettings tc;
    return QTest::qExec(&tc, argc, argv);
}
#include "test_settings.moc"
