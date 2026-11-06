// Download-sidecar bulk refresh collection: native sidecars resolve to
// (site, id) via the folder name; junk is skipped. No network here.
#include "sidecarrefresh.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

namespace {
void writeJson(const QString &path, const QJsonObject &o) {
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

QMap<QString, QString> labels() {
    return {{"Danbooru", "danbooru"}, {"Gelbooru", "gelbooru"}};
}
} // namespace

class TestSidecarRefresh : public QObject {
    Q_OBJECT
private slots:
    void collectsNative() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        QDir().mkpath(tmp.filePath(QStringLiteral("Danbooru")));
        QDir().mkpath(tmp.filePath(QStringLiteral("Gelbooru")));
        QJsonObject d;
        d[QStringLiteral("id")] = 42;
        d[QStringLiteral("tag_string_general")] = QStringLiteral("solo");
        writeJson(tmp.filePath(QStringLiteral("Danbooru/Danbooru_abc.json")), d);
        QJsonObject g;
        g[QStringLiteral("id")] = 7;
        g[QStringLiteral("tags")] = QStringLiteral("sky");
        writeJson(tmp.filePath(QStringLiteral("Gelbooru/Gelbooru_def.json")), g);
        const auto jobs = SidecarRefresh::collect(tmp.path(), labels());
        QCOMPARE(jobs.size(), 2);
        QMap<QString, qint64> bySite;
        for (const auto &j : jobs)
            bySite.insert(j.siteKey, j.id);
        QCOMPARE(bySite.value(QStringLiteral("danbooru")), 42LL);
        QCOMPARE(bySite.value(QStringLiteral("gelbooru")), 7LL);
        for (const auto &j : jobs)
            QVERIFY(j.jsonPath.endsWith(QStringLiteral(".json")));
    }

    void skipsJunk() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        QDir().mkpath(tmp.filePath(QStringLiteral("Danbooru")));
        QDir().mkpath(tmp.filePath(QStringLiteral("Unknown")));
        // Unknown folder, bad JSON, missing id, string id.
        QJsonObject good;
        good[QStringLiteral("id")] = 1;
        writeJson(tmp.filePath(QStringLiteral("Danbooru/Danbooru_a.json")), good);
        writeJson(tmp.filePath(QStringLiteral("Unknown/U_b.json")), good);
        QFile bad(tmp.filePath(QStringLiteral("Danbooru/bad.json")));
        QVERIFY(bad.open(QIODevice::WriteOnly));
        bad.write("{not json");
        bad.close();
        QJsonObject noid;
        noid[QStringLiteral("tags")] = QStringLiteral("x");
        writeJson(tmp.filePath(QStringLiteral("Danbooru/noid.json")), noid);
        QJsonObject sid;
        sid[QStringLiteral("id")] = QStringLiteral("abc");
        writeJson(tmp.filePath(QStringLiteral("Danbooru/sid.json")), sid);
        const auto jobs = SidecarRefresh::collect(tmp.path(), labels());
        QCOMPARE(jobs.size(), 1);
        QCOMPARE(jobs.first().id, 1LL);
    }

    void skipsStrays() {
        // Hand-tagged strays have no source to refresh from, by design -
        // even with a valid id and a known folder shape.
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        QDir().mkpath(tmp.filePath(QStringLiteral("_Stray")));
        QJsonObject s;
        s[QStringLiteral("id")] = 5;
        s[QStringLiteral("tags")] = QStringLiteral("sky");
        s[QStringLiteral("md5")] =
            QStringLiteral("d41d8cd98f00b204e9800998ecf8427e");
        s[QStringLiteral("stray")] = true;
        writeJson(tmp.filePath(QStringLiteral(
                      "_Stray/_Stray_d41d8cd98f00b204e9800998ecf8427e.json")),
                  s);
        QCOMPARE(SidecarRefresh::collect(tmp.path(), labels()).size(), 0);
    }

    void scopeFiltersBySiteLabel() {
        // Scoped refresh: only the chosen site subfolders collect jobs.
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        auto writeNative = [&](const QString &site, const QString &stem) {
            QDir().mkpath(tmp.filePath(site));
            QJsonObject o;
            o[QStringLiteral("id")] = 5;
            o[QStringLiteral("tag_string")] = QStringLiteral("sky");
            writeJson(tmp.filePath(site + u'/' + stem + QStringLiteral(".json")), o);
        };
        writeNative(QStringLiteral("Danbooru"), QStringLiteral("Danbooru_a"));
        writeNative(QStringLiteral("Gelbooru"), QStringLiteral("Gelbooru_b"));
        QCOMPARE(SidecarRefresh::collect(tmp.path(), labels()).size(), 2);
        const auto scoped = SidecarRefresh::collect(
            tmp.path(), labels(), {QStringLiteral("Gelbooru")});
        QCOMPARE(scoped.size(), 1);
        QCOMPARE(scoped.first().siteKey, QStringLiteral("gelbooru"));
        QVERIFY(SidecarRefresh::collect(tmp.path(), labels(),
                                        {QStringLiteral("Nope")})
                    .isEmpty());
    }

    void collectsHostFolders() {
        // Host layout (see RemotePosts::fsTagForPost): folders named by
        // base-URL host resolve even with an empty label map, and legacy
        // label folders keep working alongside.
        BooruSite dan;
        dan.key = QStringLiteral("danbooru");
        dan.label = QStringLiteral("Danbooru");
        dan.baseUrl = QStringLiteral("https://danbooru.donmai.us");
        dan.engine = BooruEngine::Danbooru;
        RemoteSource::setConfiguredBoorus({dan});
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        QDir().mkpath(tmp.filePath(QStringLiteral("danbooru.donmai.us")));
        QDir().mkpath(tmp.filePath(QStringLiteral("Danbooru")));
        QJsonObject h;
        h[QStringLiteral("id")] = 9;
        writeJson(tmp.filePath(QStringLiteral(
                      "danbooru.donmai.us/danboorudonmaius_abc.json")),
                  h);
        QJsonObject l;
        l[QStringLiteral("id")] = 10;
        writeJson(tmp.filePath(QStringLiteral("Danbooru/Danbooru_def.json")),
                  l);
        const auto jobs = SidecarRefresh::collect(tmp.path(), labels());
        RemoteSource::setConfiguredBoorus({});
        QCOMPARE(jobs.size(), 2);
        QMap<QString, qint64> bySite;
        for (const auto &j : jobs)
            bySite.insert(j.jsonPath, j.id);
        QCOMPARE(bySite.value(tmp.filePath(QStringLiteral(
                                  "danbooru.donmai.us/danboorudonmaius_abc.json"))),
                 9LL);
        QCOMPARE(bySite.value(tmp.filePath(
                                  QStringLiteral("Danbooru/Danbooru_def.json"))),
                 10LL);
    }

    void registryLabelMap() {        // Configured entries resolve; custom labels shadow by key.
        BooruSite dan;
        dan.key = QStringLiteral("danbooru");
        dan.label = QStringLiteral("Danbooru");
        dan.shortName = QStringLiteral("DAN");
        dan.baseUrl = QStringLiteral("https://danbooru.donmai.us");
        dan.engine = BooruEngine::Danbooru;
        BooruSite szu;
        szu.key = QStringLiteral("szurubooru");
        szu.label = QStringLiteral("Szurubooru");
        szu.shortName = QStringLiteral("SZU");
        szu.baseUrl = QStringLiteral("https://szu.example");
        szu.engine = BooruEngine::Szurubooru;
        RemoteSource::setConfiguredBoorus({dan, szu});
        const QMap<QString, QString> m = RemoteSource::labelKeys();
        QCOMPARE(m.value(QStringLiteral("Danbooru")), QStringLiteral("danbooru"));
        QCOMPARE(m.value(QStringLiteral("Szurubooru")), QStringLiteral("szurubooru"));
        BooruSite custom;
        custom.key = QStringLiteral("gelbooru");
        custom.label = QStringLiteral("My Gelbooru");
        custom.baseUrl = QStringLiteral("https://g.example");
        custom.engine = BooruEngine::Gelbooru;
        RemoteSource::setConfiguredBoorus({custom});
        QCOMPARE(RemoteSource::labelKeys().value(QStringLiteral("My Gelbooru")),
                 QStringLiteral("gelbooru"));
        RemoteSource::setConfiguredBoorus({});
    }

    void collectGaps() {
        // src/library/sidecarrefresh.cpp:10-56
        // src/remote/remotesource.cpp:931 (hostKeys)
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        QDir().mkpath(tmp.filePath(QStringLiteral("Danbooru")));
        QJsonObject o;
        o[QStringLiteral("id")] = 42;
        o[QStringLiteral("tag_string")] = QStringLiteral("sky");
        writeJson(tmp.filePath(QStringLiteral("Danbooru/Danbooru_a.json")), o);

        // id as string "42" is parsed by toLongLong -> passes
        QJsonObject sid;
        sid[QStringLiteral("id")] = QStringLiteral("42");
        writeJson(tmp.filePath(QStringLiteral("Danbooru/Danbooru_sid.json")), sid);
        // id as 0 -> skipped
        QJsonObject zero;
        zero[QStringLiteral("id")] = 0;
        writeJson(tmp.filePath(QStringLiteral("Danbooru/Danbooru_zero.json")), zero);
        // id as -1 -> skipped
        QJsonObject neg;
        neg[QStringLiteral("id")] = -1;
        writeJson(tmp.filePath(QStringLiteral("Danbooru/Danbooru_neg.json")), neg);
        // stray as 1 -> skipped
        QJsonObject strayInt;
        strayInt[QStringLiteral("id")] = 7;
        strayInt[QStringLiteral("stray")] = true;
        writeJson(tmp.filePath(QStringLiteral("Danbooru/Danbooru_stray.json")), strayInt);
        // stray as string "true" -> NOT skipped (toBool returns false for strings)
        QJsonObject strayStr;
        strayStr[QStringLiteral("id")] = 8;
        strayStr[QStringLiteral("stray")] = QStringLiteral("true");
        writeJson(tmp.filePath(QStringLiteral("Danbooru/Danbooru_straystr.json")), strayStr);
        // JSON array -> skipped
        QJsonArray arr;
        arr.append(1);
        QFile arrf(tmp.filePath(QStringLiteral("Danbooru/arr.json")));
        QVERIFY(arrf.open(QIODevice::WriteOnly));
        arrf.write(QJsonDocument(arr).toJson(QJsonDocument::Compact));
        arrf.close();
        // empty download dir -> empty list
        QVERIFY(SidecarRefresh::collect(tmp.filePath(QStringLiteral("nope")), labels()).isEmpty());

        const auto jobs = SidecarRefresh::collect(tmp.path(), labels());
        // a (42) + sid (42) + straystr (8) = 3
        QCOMPARE(jobs.size(), 3);

        // siteLabels empty -> collects all known folders
        const auto all = SidecarRefresh::collect(tmp.path(), labels(), {});
        QCOMPARE(all.size(), 3);

        // siteLabels with unknown folder -> skips all
        const auto none = SidecarRefresh::collect(tmp.path(), labels(),
                                                  {QStringLiteral("Nope")});
        QVERIFY(none.isEmpty());

        // hostKeys: configured entries win on collision, presets included
        BooruSite dan;
        dan.key = QStringLiteral("danbooru");
        dan.label = QStringLiteral("Danbooru");
        dan.baseUrl = QStringLiteral("https://danbooru.donmai.us");
        dan.engine = BooruEngine::Danbooru;
        RemoteSource::setConfiguredBoorus({dan});
        const QMap<QString, QString> hk = RemotePosts::hostKeys();
        QVERIFY(hk.contains(QStringLiteral("danbooru.donmai.us")));
        QCOMPARE(hk.value(QStringLiteral("danbooru.donmai.us")), QStringLiteral("danbooru"));
        // preset booru (Safebooru) included
        QVERIFY(hk.contains(QStringLiteral("safebooru.donmai.us")));
        RemoteSource::setConfiguredBoorus({});
    }
};

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    TestSidecarRefresh tc;
    return QTest::qExec(&tc, argc, argv);
}
#include "test_sidecarrefresh.moc"
