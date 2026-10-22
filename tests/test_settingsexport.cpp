// Settings backup/restore JSON round-trip (no files, no network).
#include "settingsexport.h"

#include "settings.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QtTest>

namespace {
AppSettings sampleSettings() {
    AppSettings s;
    s.browseSites = {QStringLiteral("danbooru")};
    s.browseLimit = 60;
    s.wallAutoPopup = false;
    s.customBucketMap.insert(QStringLiteral("flag"), QStringLiteral("meta"));
    s.favAutoDownload = true;
    s.blacklist = {QStringLiteral("loli")};
    s.hideAi = true;
    s.favTags = {QStringLiteral("sky")};
    BooruSite b;
    b.key = QStringLiteral("danbooru");
    b.label = QStringLiteral("Danbooru");
    b.baseUrl = QStringLiteral("https://danbooru.donmai.us");
    b.engine = BooruEngine::Danbooru;
    b.badgeColor = QStringLiteral("#123456");
    b.skipAutoClassify = true;
    b.localBlacklist = true;
    s.boorus = {b};
    s.disabledBuckets = {QStringLiteral("gelbooru:character")};
    s.manualTagTypes.insert(QStringLiteral("sky"), QStringLiteral("general"));
    s.ugoiraFormat = QStringLiteral("jxl");
    s.ugoiraFrameDelayMs = 150;
    return s;
}
Post sampleFav() {
    Post p;
    p.isRemote = true;
    p.siteName = QStringLiteral("Danbooru");
    p.id = 4242;
    p.rating = QStringLiteral("general");
    p.ext = QStringLiteral("jpg");
    p.tags = p.tagsGeneral = QStringLiteral("1girl sky");
    p.fileUrl = QStringLiteral("https://cdn.donmai.us/f.jpg");
    p.postUrl = QStringLiteral("https://danbooru.donmai.us/posts/4242");
    p.hasMeta = true;
    return p;
}
} // namespace

class TestSettingsExport : public QObject {
    Q_OBJECT
private slots:
    void roundTrip() {
        const QJsonObject doc =
            SettingsBackup::exportJson(sampleSettings(), {sampleFav()});
        QCOMPARE(doc.value(QStringLiteral("app")).toString(), QStringLiteral("box"));
        AppSettings back;
        QVector<Post> favs;
        QString err;
        QVERIFY(SettingsBackup::importJson(doc, &back, &favs, &err));
        QVERIFY(err.isEmpty());
        QCOMPARE(back.browseSites, QStringList({QStringLiteral("danbooru")}));
        QCOMPARE(back.browseLimit, 60);
        QVERIFY(!back.wallAutoPopup);
        QCOMPARE(back.customBucketMap.value(QStringLiteral("flag")), QStringLiteral("meta"));
        QVERIFY(back.favAutoDownload);
        QCOMPARE(back.blacklist, QStringList({QStringLiteral("loli")}));
        QVERIFY(back.hideAi);
        QCOMPARE(back.favTags, QStringList({QStringLiteral("sky")}));
        QCOMPARE(back.boorus.size(), 1);
        QCOMPARE(back.boorus.first().key, QStringLiteral("danbooru"));
        QCOMPARE(back.boorus.first().badgeColor, QStringLiteral("#123456"));
        QVERIFY(back.boorus.first().skipAutoClassify);
        QVERIFY(back.boorus.first().localBlacklist);
        QVERIFY(back.disabledBuckets.contains(QStringLiteral("gelbooru:character")));
        QCOMPARE(back.manualTagTypes.value(QStringLiteral("sky")), QStringLiteral("general"));
        QCOMPARE(back.ugoiraFormat, QStringLiteral("jxl"));
        QCOMPARE(back.ugoiraFrameDelayMs, 150);
        QCOMPARE(favs.size(), 1);
        QCOMPARE(favs.first().id, 4242LL);
        QCOMPARE(favs.first().siteName, QStringLiteral("Danbooru"));
        // Re-export is stable.
        QCOMPARE(SettingsBackup::exportJson(back, favs),
                 SettingsBackup::exportJson(sampleSettings(), {sampleFav()}));
    }

    void rejectsGarbage() {
        AppSettings back;
        QVector<Post> favs;
        QString err;
        QVERIFY(!SettingsBackup::importJson(QJsonObject(), &back, &favs, &err));
        QVERIFY(!err.isEmpty());
        QJsonObject wrong;
        wrong[QStringLiteral("app")] = QStringLiteral("other");
        wrong[QStringLiteral("format")] = 1;
        QVERIFY(!SettingsBackup::importJson(wrong, &back, &favs, nullptr));
        QJsonObject future;
        future[QStringLiteral("app")] = QStringLiteral("box");
        future[QStringLiteral("format")] = 999;
        QVERIFY(!SettingsBackup::importJson(future, &back, &favs, nullptr));
    }

    void boorusMergeByKey() {
        AppSettings live;
        BooruSite keep;
        keep.key = QStringLiteral("mine");
        keep.label = QStringLiteral("Mine");
        BooruSite stale;
        stale.key = QStringLiteral("danbooru");
        stale.label = QStringLiteral("Old");
        live.boorus = {keep, stale};
        BooruSite upd;
        upd.key = QStringLiteral("danbooru");
        upd.label = QStringLiteral("Danbooru!");
        upd.baseUrl = QStringLiteral("https://danbooru.donmai.us");
        QJsonObject o;
        o[QStringLiteral("app")] = QStringLiteral("box");
        o[QStringLiteral("format")] = 1;
        QJsonArray arr;
        QJsonObject updJson;
        updJson[QStringLiteral("key")] = QStringLiteral("danbooru");
        updJson[QStringLiteral("label")] = QStringLiteral("Danbooru!");
        arr.append(updJson);
        QJsonObject newJson;
        newJson[QStringLiteral("key")] = QStringLiteral("newbooru");
        newJson[QStringLiteral("label")] = QStringLiteral("New");
        arr.append(newJson);
        o[QStringLiteral("boorus")] = arr;
        QVector<Post> favs;
        QVERIFY(SettingsBackup::importJson(o, &live, &favs, nullptr));
        QCOMPARE(live.boorus.size(), 3); // mine kept, danbooru updated, newbooru added
        for (const BooruSite &b : live.boorus) {
            if (b.key == QStringLiteral("danbooru"))
                QCOMPARE(b.label, QStringLiteral("Danbooru!"));
        }
        QVERIFY(favs.isEmpty());
    }
};

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    TestSettingsExport tc;
    return QTest::qExec(&tc, argc, argv);
}
#include "test_settingsexport.moc"
