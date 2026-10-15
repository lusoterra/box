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

    void registryLabelMap() {
        // Configured entries resolve; custom labels shadow by key.
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
};

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    TestSidecarRefresh tc;
    return QTest::qExec(&tc, argc, argv);
}
#include "test_sidecarrefresh.moc"
