// Settings persistence: defaults, round-trip, effective dirs, legacy
// booru-credential migration. Fully isolated via
// XDG_CONFIG_HOME/XDG_DATA_HOME temp dirs (never touches real config).
#include "settings.h"
#include "tagtypes.h"

#include <QTemporaryDir>
#include <QCoreApplication>
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
        QVERIFY(s.indexCache && s.thumbCache);
        QVERIFY(!s.fullCache); // silent full downloads default OFF
        QCOMPARE(s.cacheLimitMB, 10240);
        QVERIFY(s.videoMuted && s.videoLoop);
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
        s.cacheLimitMB = 512;
        s.thumbLongSide = 512;
        s.lastRoot = QStringLiteral("/pics/dump");
        s.history = {QStringLiteral("/pics/a"), QStringLiteral("/pics/b")};
        s.downloadDir = QStringLiteral("/tmp/be-dl");
        s.dataDir = QStringLiteral("/tmp/be-data");
        s.globalProxy = QStringLiteral("socks5h://127.0.0.1:9050");
        s.favAutoDownload = true;
        s.favAutoFav = true;
        s.unfavDeletesFile = true;
        s.deleteUnfavs = true;
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
        s.boorus = {custom};
        s.explicitTags = {QStringLiteral("customtag")};
        s.disabledBuckets = {QStringLiteral("gelbooru:character")};
        s.volume = 42;
        s.aspectNum = 4;
        s.aspectDen = 3;
        s.gridBoxW = 256;
        s.gridScrollRows = 2.5;
        s.tabsDefault = QStringLiteral("last");
        s.language = QStringLiteral("fr");
        s.mascotPath = QStringLiteral("/pics/mascot.png");
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
        QCOMPARE(back.cacheLimitMB, 512);
        QCOMPARE(back.thumbLongSide, 512);
        QCOMPARE(back.lastRoot, QStringLiteral("/pics/dump"));
        QCOMPARE(back.history, s.history);
        QCOMPARE(back.downloadDir, QStringLiteral("/tmp/be-dl"));
        QCOMPARE(back.dataDir, QStringLiteral("/tmp/be-data"));
        QCOMPARE(back.globalProxy,
                 QStringLiteral("socks5h://127.0.0.1:9050"));
        QVERIFY(back.favAutoDownload);
        QVERIFY(back.favAutoFav);
        QVERIFY(back.unfavDeletesFile);
        QVERIFY(back.deleteUnfavs);
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
        QCOMPARE(back.disabledBuckets,
                 QSet<QString>({QStringLiteral("gelbooru:character")}));
        QCOMPARE(back.explicitTags, QStringList({QStringLiteral("customtag")}));
        QCOMPARE(back.volume, 42);
        QCOMPARE(back.aspectNum, 4);
        QCOMPARE(back.aspectDen, 3);
        QCOMPARE(back.gridBoxW, 256);
        QCOMPARE(back.gridScrollRows, 2.5);
        QCOMPARE(back.tabsDefault, QStringLiteral("last"));
        QCOMPARE(back.language, QStringLiteral("fr"));
        QCOMPARE(back.mascotPath, QStringLiteral("/pics/mascot.png"));
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

    void legacyCredsMigrateToBoorus() {
        // Simulate a pre-registry config: legacy keys, no boorus/order.
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
        QCOMPARE(back.boorus.first().baseUrl, QStringLiteral("https://safebooru.org"));
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
        QCOMPARE(back.savedTabs.at(2).kind, QStringLiteral("favs"));
        QCOMPARE(back.savedTabCurrent, 2);
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
};

int main(int argc, char *argv[]) {
    // QCoreApplication (not appless): QSettings path handling and friends
    // require an application instance. Still no display needed.
    QCoreApplication app(argc, argv);
    TestSettings tc;
    return QTest::qExec(&tc, argc, argv);
}
#include "test_settings.moc"
