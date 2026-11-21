// Library statistics: favourites aggregation, download-folder scan,
// favourite/download dedupe and cache tree accounting.
#include "statsdialog.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>
#include <QTextBrowser>

namespace {
Post makePost(const QString &site, qint64 id, const QString &tags,
              const QString &rating) {
    Post p;
    p.siteName = site;
    p.id = id;
    p.tags = tags;
    p.tagsGeneral = tags;
    p.rating = rating;
    return p;
}

QString writeFile(const QString &path, const QByteArray &bytes) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return {};
    f.write(bytes);
    f.close();
    return path;
}

QByteArray sidecarJson(qint64 id, const QString &general,
                       const QString &copyright, const QString &fileUrl) {
    QJsonObject o;
    o[QStringLiteral("id")] = id;
    o[QStringLiteral("tag_string_general")] = general;
    if (!copyright.isEmpty())
        o[QStringLiteral("tag_string_copyright")] = copyright;
    o[QStringLiteral("rating")] = QStringLiteral("general");
    o[QStringLiteral("file_url")] = fileUrl;
    return QJsonDocument(o).toJson();
}
} // namespace

class TestStats : public QObject {
    Q_OBJECT
private slots:
    void favouritesAggregate() {
        const QVector<Post> favs = {
            makePost(QStringLiteral("Danbooru"), 1,
                     QStringLiteral("1girl lucky_star"), QStringLiteral("explicit")),
            makePost(QStringLiteral("Danbooru"), 2, QStringLiteral("1girl solo"),
                     QStringLiteral("general"))};
        const StatsReport r = statsFromFavourites(favs);
        QCOMPARE(r.favourites, 2);
        QCOMPARE(r.libraryPosts, 2);
        QCOMPARE(r.tagCounts.value(QStringLiteral("1girl")), 2);
        QCOMPARE(r.tagCounts.value(QStringLiteral("lucky_star")), 1);
        QCOMPARE(r.ratingCounts.value(QStringLiteral("explicit")), 1);
        QCOMPARE(r.ratingCounts.value(QStringLiteral("general")), 1);
        // Site resolution depends on the registry (test config may differ);
        // the label fallback must still keep both rows together.
        QCOMPARE(r.booruCounts.value(QStringLiteral("Danbooru")), 2);
    }

    void scansDownloadFolder() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QDir root(tmp.path());
        QVERIFY(root.mkpath(QStringLiteral("downloads/danbooru.donmai.us")));
        QVERIFY(root.mkpath(QStringLiteral("downloads/_Stray")));
        const QString host = tmp.path() + QStringLiteral("/downloads/danbooru.donmai.us");
        const QString stray = tmp.path() + QStringLiteral("/downloads/_Stray");
        QVERIFY(!writeFile(host + QStringLiteral("/media_abc.png"), "png").isEmpty());
        QVERIFY(!writeFile(host + QStringLiteral("/media_abc.json"),
                           sidecarJson(5, QStringLiteral("1girl solo"),
                                       QStringLiteral("lucky_star"),
                                       QStringLiteral("https://example.test/5.png")))
                     .isEmpty());
        QVERIFY(!writeFile(stray + QStringLiteral("/plain.jpg"), "jpg").isEmpty());

        StatsScanInput in;
        in.downloadDir = tmp.path() + QStringLiteral("/downloads");
        in.cacheDir = tmp.path() + QStringLiteral("/cache");
        const StatsScanResult r = scanStatsPaths(in);
        QCOMPARE(r.downloadedFiles, 2);
        QCOMPARE(r.downloadedPosts, 2);
        QCOMPARE(r.images, 2);
        QCOMPARE(r.videos, 0);
        QCOMPARE(r.tagCounts.value(QStringLiteral("1girl")), 1);
        QCOMPARE(r.tagCounts.value(QStringLiteral("solo")), 1);
        QVERIFY(r.tagCounts.value(QStringLiteral("lucky_star")) >= 1);
        QCOMPARE(r.ratingCounts.value(QStringLiteral("general")), 1);
    }

    void downloadedFavouriteIsNotDoubleCounted() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QDir root(tmp.path());
        QVERIFY(root.mkpath(QStringLiteral("downloads/danbooru.donmai.us")));
        const QString host = tmp.path() + QStringLiteral("/downloads/danbooru.donmai.us");
        QVERIFY(!writeFile(host + QStringLiteral("/media_abc.png"), "png").isEmpty());
        QVERIFY(!writeFile(host + QStringLiteral("/media_abc.json"),
                           sidecarJson(5, QStringLiteral("1girl"), QString(),
                                       QStringLiteral("https://example.test/5.png")))
                     .isEmpty());

        Post fav = makePost(QStringLiteral("Danbooru"), 5,
                            QStringLiteral("1girl"), QStringLiteral("general"));
        fav.fileUrl = QStringLiteral("https://example.test/5.png");
        StatsScanInput in;
        in.downloadDir = tmp.path() + QStringLiteral("/downloads");
        in.favouriteIds = statsPostKeys({fav});
        const StatsScanResult r = scanStatsPaths(in);
        QCOMPARE(r.downloadedFiles, 1);
        QCOMPARE(r.downloadedPosts, 0); // already in favourites
        QVERIFY(r.tagCounts.isEmpty());
    }

    void cacheTreeIsSplit() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString cache = tmp.path() + QStringLiteral("/cache");
        QVERIFY(QDir().mkpath(cache + QStringLiteral("/thumbs")));
        QVERIFY(QDir().mkpath(cache + QStringLiteral("/full")));
        QVERIFY(QDir().mkpath(cache + QStringLiteral("/roots/abc")));
        QVERIFY(!writeFile(cache + QStringLiteral("/thumbs/a.png"), "1234").isEmpty());
        QVERIFY(!writeFile(cache + QStringLiteral("/full/b.png"), "12345678").isEmpty());
        QVERIFY(!writeFile(cache + QStringLiteral("/roots/abc/index.db"), "123456")
                     .isEmpty());
        QVERIFY(!writeFile(cache + QStringLiteral("/other.bin"), "12").isEmpty());

        StatsScanInput in;
        in.cacheDir = cache;
        const StatsScanResult r = scanStatsPaths(in);
        QVERIFY(r.cacheScanned);
        QCOMPARE(r.thumbFiles, 1);
        QCOMPARE(r.thumbBytes, qint64(4));
        QCOMPARE(r.fullFiles, 1);
        QCOMPARE(r.fullBytes, qint64(8));
        QCOMPARE(r.indexFiles, 1);
        QCOMPARE(r.indexBytes, qint64(6));
        QCOMPARE(r.cacheBytes, qint64(4 + 8 + 6 + 2));
    }

    void dialogRendersAndMergesScan() {
        StatsReport r;
        r.favourites = 2;
        r.downloadedFiles = 3;
        r.downloadedPosts = 1;
        r.downloadedBytes = 2048;
        r.libraryPosts = 3;
        r.tagCounts[QStringLiteral("1girl")] = 3;
        r.artistCounts[QStringLiteral("some_artist")] = 1;
        r.booruCounts[QStringLiteral("Danbooru")] = 2;
        r.ratingCounts[QStringLiteral("general")] = 2;
        r.cachedTagRows = 42;
        r.cacheScanned = true;
        r.cacheBytes = 4096;
        r.thumbFiles = 5;
        r.archive.posts = 4;
        r.archive.topTags = {{QStringLiteral("1girl"), 4}};

        StatsDialog dlg(r);
        auto *view = dlg.findChild<QTextBrowser *>();
        QVERIFY(view);
        QString text = view->toPlainText();
        QVERIFY2(text.contains(QStringLiteral("Most common tags")),
                 qPrintable(text));
        QVERIFY(text.contains(QStringLiteral("Most used boorus")));
        QVERIFY(text.contains(QStringLiteral("Danbooru")));
        QVERIFY(text.contains(QStringLiteral("Open archive")));
        QVERIFY(text.contains(QStringLiteral("42"))); // cached tag rows
        QVERIFY(text.contains(QStringLiteral("Reading the download folder")));

        StatsScanResult scan;
        scan.cacheScanned = true;
        scan.downloadedFiles = 2;
        scan.downloadedPosts = 1;
        scan.downloadedBytes = 512;
        scan.tagCounts[QStringLiteral("solo")] = 1;
        dlg.mergeScan(scan);
        text = view->toPlainText();
        QVERIFY(!text.contains(QStringLiteral("Reading the download folder")));
        QVERIFY(text.contains(QStringLiteral("solo"))); // merged download tag
        QVERIFY(text.contains(QStringLiteral("1girl")));
    }

    void topEntriesRanking() {
        QHash<QString, int> counts;
        counts[QStringLiteral("1girl")] = 500;
        counts[QStringLiteral("lucky_star")] = 400;
        counts[QStringLiteral("solo")] = 400;
        const QVector<StatEntry> top = statsTopEntries(counts, 2);
        QCOMPARE(top.size(), 2);
        QCOMPARE(top.at(0).label, QStringLiteral("1girl"));
        QCOMPARE(top.at(0).count, 500);
        // Tie on count: alphabetical keeps the list stable.
        QCOMPARE(top.at(1).label, QStringLiteral("lucky_star"));
    }
};

QTEST_MAIN(TestStats)
#include "test_stats.moc"
