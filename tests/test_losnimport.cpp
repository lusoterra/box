// LoliSnatcher backup import: host/site mapping, post-id + md5 URL
// parsing, gallery-dl-shaped fallback sidecars, synthetic store.db reads.
#include "losnimport.h"
#include "metaparser.h"

#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest>

namespace {
void makeBackup(const QString &path) {
    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("t_mkbk"));
    db.setDatabaseName(path);
    QVERIFY(db.open());
    QSqlQuery q(db);
    QVERIFY(q.exec(QStringLiteral("CREATE TABLE Tag (id INTEGER PRIMARY KEY, name TEXT)")));
    QVERIFY(q.exec(QStringLiteral("CREATE TABLE ImageTag (tagID INTEGER, booruItemID INTEGER)")));
    QVERIFY(q.exec(QStringLiteral("CREATE TABLE BooruItem (id INTEGER PRIMARY KEY,"
                                  " thumbnailURL TEXT, sampleURL TEXT, fileURL TEXT,"
                                  " postURL TEXT, mediaType TEXT,"
                                  " isSnatched INTEGER, isFavourite INTEGER)")));
    QVERIFY(q.exec(QStringLiteral("INSERT INTO Tag VALUES (1, '1girl'), (2, 'sky')")));
    QVERIFY(q.exec(QStringLiteral(
        "INSERT INTO BooruItem VALUES"
        " (10, 'thumb1', 'sample1',"
        "  'https://img4.gelbooru.com/images/51/6e/516e1f01a05ea895b33316bd28bfd27f.jpg',"
        "  'https://gelbooru.com/index.php?page=post&s=view&id=7103104',"
        "  'image', 1, 1),"
        " (11, 'thumb2', 'sample2', 'https://example.com/x.png',"
        "  'https://danbooru.donmai.us/posts/5', 'image', 0, 1)")));
    QVERIFY(q.exec(QStringLiteral("INSERT INTO ImageTag VALUES (1, 10), (2, 10), (1, 11)")));
    db.close();
    QSqlDatabase::removeDatabase(QStringLiteral("t_mkbk"));
}
} // namespace

class TestLosnImport : public QObject {
    Q_OBJECT
private slots:
    void siteForKnownHosts() {
        QString key, label;
        LosnImport::siteForHost(QStringLiteral("danbooru.donmai.us"), key, label);
        QCOMPARE(key, QStringLiteral("danbooru"));
        QCOMPARE(label, QStringLiteral("Danbooru"));
        LosnImport::siteForHost(QStringLiteral("gelbooru.com"), key, label);
        QCOMPARE(key, QStringLiteral("gelbooru"));
        LosnImport::siteForHost(QStringLiteral("safebooru.org"), key, label);
        QCOMPARE(key, QStringLiteral("safebooru"));
        LosnImport::siteForHost(QStringLiteral("yande.re"), key, label);
        QCOMPARE(key, QStringLiteral("yandere"));
        LosnImport::siteForHost(QStringLiteral("konachan.com"), key, label);
        QCOMPARE(key, QStringLiteral("konachan"));
        LosnImport::siteForHost(QStringLiteral("aibooru.online"), key, label);
        QCOMPARE(key, QStringLiteral("aibooru"));
        LosnImport::siteForHost(QStringLiteral("cdn.donmai.us"), key, label);
        QCOMPARE(key, QStringLiteral("danbooru"));
        QCOMPARE(label, QStringLiteral("Danbooru"));
    }

    void siteForUnknownHostDerivesLabel() {
        QString key, label;
        LosnImport::siteForHost(QStringLiteral("rule34.xxx"), key, label);
        QVERIFY(key.isEmpty());
        QCOMPARE(label, QStringLiteral("Rule34"));
        LosnImport::siteForHost(QString(), key, label);
        QVERIFY(key.isEmpty());
        QCOMPARE(label, QStringLiteral("LoliSnatcher"));
    }

    void idFromPostUrls() {
        QCOMPARE(LosnImport::idFromPostUrl(QStringLiteral("https://danbooru.donmai.us/posts/12345")),
                 12345LL);
        QCOMPARE(LosnImport::idFromPostUrl(QStringLiteral("https://yande.re/post/show/99")),
                 99LL);
        QCOMPARE(LosnImport::idFromPostUrl(
                     QStringLiteral("https://gelbooru.com/index.php?page=post&s=view&id=77")),
                 77LL);
        QCOMPARE(LosnImport::idFromPostUrl(QStringLiteral("https://example.com/noid")),
                 0LL);
        QCOMPARE(LosnImport::idFromPostUrl(QString()), 0LL);
    }

    void md5FromFileUrls() {
        QCOMPARE(LosnImport::md5FromFileUrl(
                     QStringLiteral("https://img4.gelbooru.com/images/51/6e/"
                                    "516e1f01a05ea895b33316bd28bfd27f.jpg")),
                 QStringLiteral("516e1f01a05ea895b33316bd28bfd27f"));
        // Moebooru keeps the hash in a path segment, not the file name.
        QCOMPARE(LosnImport::md5FromFileUrl(
                     QStringLiteral("https://files.yande.re/image/"
                                    "6eefea0ac79c8e3d01d0b7dabe4389eb/"
                                    "yande.re%201215079%20tags.png")),
                 QStringLiteral("6eefea0ac79c8e3d01d0b7dabe4389eb"));
        // Uppercase hashes normalise to lowercase (disk index is lower).
        QCOMPARE(LosnImport::md5FromFileUrl(
                     QStringLiteral("https://x.test/images/"
                                    "516E1F01A05EA895B33316BD28BFD27F.JPG")),
                 QStringLiteral("516e1f01a05ea895b33316bd28bfd27f"));
        QVERIFY(LosnImport::md5FromFileUrl(QStringLiteral("https://example.com/x.png")).isEmpty());
        QVERIFY(LosnImport::md5FromFileUrl(QString()).isEmpty());
    }

    void diskIndexMatchesSidecarUrls() {
        // A file renamed after hashing (no hash in the backup URL) is
        // still "already downloaded" through its sidecar's file/post URL.
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        QDir(tmp.path()).mkpath(QStringLiteral("yande.re"));
        const QString media =
            tmp.filePath(QStringLiteral("yande.re/yandere_deadbeef.png"));
        QFile f(media);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("x");
        f.close();
        QJsonObject sc;
        sc[QStringLiteral("file_url")] =
            QStringLiteral("https://files.yande.re/image/abc/"
                           "yande.re%20123%20tags.png?token=1");
        sc[QStringLiteral("post_url")] =
            QStringLiteral("https://yande.re/post/show/123");
        QFile jf(tmp.filePath(QStringLiteral("yande.re/yandere_deadbeef.json")));
        QVERIFY(jf.open(QIODevice::WriteOnly));
        jf.write(QJsonDocument(sc).toJson());
        jf.close();
        // gallery-dl flat sidecar form: <media>.json.
        const QString flat = tmp.filePath(
            QStringLiteral("yande.re/other.jpg"));
        QFile mf(flat);
        QVERIFY(mf.open(QIODevice::WriteOnly));
        mf.write("y");
        mf.close();
        QJsonObject flatSc;
        flatSc[QStringLiteral("file_url")] =
            QStringLiteral("https://files.yande.re/image/def/other.jpg");
        QFile ff(flat + QStringLiteral(".json"));
        QVERIFY(ff.open(QIODevice::WriteOnly));
        ff.write(QJsonDocument(flatSc).toJson());
        ff.close();

        const LosnImport::DiskIndex idx = LosnImport::diskIndex(tmp.path());
        // Scheme and query do not matter for the URL identity.
        QCOMPARE(idx.byUrl.value(LosnImport::diskUrlKey(
                     QStringLiteral("http://files.yande.re/image/abc/"
                                    "yande.re%20123%20tags.png"))),
                 media);
        QCOMPARE(idx.byUrl.value(LosnImport::diskUrlKey(
                     QStringLiteral("https://yande.re/post/show/123/"))),
                 media);
        QCOMPARE(idx.byUrl.value(LosnImport::diskUrlKey(
                     QStringLiteral("https://files.yande.re/image/def/other.jpg"))),
                 flat);
        // A JSON with no sibling media is not a download sidecar.
        QJsonObject orphanSc;
        orphanSc[QStringLiteral("file_url")] =
            QStringLiteral("https://files.yande.re/image/999/orphan.png");
        orphanSc[QStringLiteral("post_url")] =
            QStringLiteral("https://yande.re/post/show/999");
        QFile orphan(tmp.filePath(QStringLiteral("yande.re/orphan.json")));
        QVERIFY(orphan.open(QIODevice::WriteOnly));
        orphan.write(QJsonDocument(orphanSc).toJson());
        orphan.close();
        QVERIFY(!LosnImport::diskIndex(tmp.path())
                     .byUrl.contains(LosnImport::diskUrlKey(
                         QStringLiteral("https://yande.re/post/show/999"))));
    }

    void diskUrlKeyIgnoresSchemeQueryFragment() {
        const QString a = LosnImport::diskUrlKey(
            QStringLiteral("http://Example.com/a/b.png?x=1#frag"));
        const QString b = LosnImport::diskUrlKey(
            QStringLiteral("https://example.com/a/b.png"));
        QCOMPARE(a, b);
        QCOMPARE(a, QStringLiteral("example.com/a/b.png"));
        QVERIFY(LosnImport::diskUrlKey(QString()).isEmpty());
        QVERIFY(LosnImport::diskUrlKey(QStringLiteral("not a url")).isEmpty());
        // Post URLs keep the query (Gelbooru post id), order-insensitive.
        const QString g1 = LosnImport::diskUrlKey(
            QStringLiteral("https://gelbooru.com/index.php?page=post&s=view&id=1"),
            true);
        const QString g2 = LosnImport::diskUrlKey(
            QStringLiteral("http://gelbooru.com/index.php?id=1&s=view&page=post"),
            true);
        const QString g3 = LosnImport::diskUrlKey(
            QStringLiteral("https://gelbooru.com/index.php?page=post&s=view&id=2"),
            true);
        QCOMPARE(g1, g2);
        QVERIFY(g1 != g3);
    }

    void nativeSidecarShape() {
        LosnRow row;
        row.postId = 7103104;
        row.md5 = QStringLiteral("516e1f01a05ea895b33316bd28bfd27f");
        row.tags = {QStringLiteral("1girl"), QStringLiteral("sky")};
        row.fileUrl = QStringLiteral("https://example.com/f.jpg");
        const QJsonObject sc = LosnImport::nativeSidecar(row);
        QCOMPARE(sc[QStringLiteral("id")].toDouble(), 7103104.0);
        QCOMPARE(sc[QStringLiteral("rating")].toString(), QStringLiteral("g"));
        QCOMPARE(sc[QStringLiteral("tag_string_general")].toString(),
                 QStringLiteral("1girl sky"));
        QCOMPARE(sc[QStringLiteral("md5")].toString(),
                 QStringLiteral("516e1f01a05ea895b33316bd28bfd27f"));
        QCOMPARE(sc[QStringLiteral("file_url")].toString(),
                 QStringLiteral("https://example.com/f.jpg"));
        QCOMPARE(sc[QStringLiteral("file_ext")].toString(), QStringLiteral("jpg"));
        // The scanner must accept it as a Danbooru-native object.
        Post p;
        QVERIFY(MetaParser::parse(sc, p));
        QCOMPARE(p.id, 7103104LL);
        QVERIFY(p.tagsGeneral.contains(QStringLiteral("sky")));
    }

    void targetPathsShape() {
        LosnRow row;
        row.rowId = 11;
        row.siteLabel = QStringLiteral("Gelbooru");
        row.md5 = QStringLiteral("516e1f01a05ea895b33316bd28bfd27f");
        row.fileUrl = QStringLiteral("https://example.com/f.png");
        const LosnImport::DownloadPaths p =
            LosnImport::targetPaths(row, QStringLiteral("/dest"));
        QCOMPARE(p.fileName,
                 QStringLiteral("Gelbooru_516e1f01a05ea895b33316bd28bfd27f.png"));
        QCOMPARE(p.filePath,
                 QStringLiteral("/dest/Gelbooru/"
                                "Gelbooru_516e1f01a05ea895b33316bd28bfd27f.png"));
        QCOMPARE(p.jsonPath,
                 QStringLiteral("/dest/Gelbooru/"
                                "Gelbooru_516e1f01a05ea895b33316bd28bfd27f.json"));
        // No md5: stable fallback stem (re-imports agree on the path).
        LosnRow bare;
        bare.rowId = 7;
        bare.siteLabel = QStringLiteral("Danbooru");
        bare.fileUrl = QStringLiteral("https://example.com/x");
        const LosnImport::DownloadPaths q =
            LosnImport::targetPaths(bare, QStringLiteral("/d"));
        QCOMPARE(q.fileName, QStringLiteral("Danbooru_losn7.jpg"));
        QCOMPARE(q.filePath, QStringLiteral("/d/Danbooru/Danbooru_losn7.jpg"));
    }

    void targetPathsUseHostFolderLikeDownloader() {
        // A row read from a backup knows its host: naming must match
        // DownloadManager::plan (<host>/<host-without-punctuation>_<md5>)
        // so imports never create a parallel <Label>/ tree.
        LosnRow row;
        row.rowId = 5;
        row.siteLabel = QStringLiteral("Gelbooru");
        row.host = QStringLiteral("gelbooru.com");
        row.md5 = QStringLiteral("516e1f01a05ea895b33316bd28bfd27f");
        row.fileUrl = QStringLiteral("https://img4.gelbooru.com/x.jpg");
        const LosnImport::DownloadPaths p =
            LosnImport::targetPaths(row, QStringLiteral("/dest"));
        QCOMPARE(p.fileName,
                 QStringLiteral("gelboorucom_516e1f01a05ea895b33316bd28bfd27f.jpg"));
        QCOMPARE(p.filePath,
                 QStringLiteral("/dest/gelbooru.com/"
                                "gelboorucom_516e1f01a05ea895b33316bd28bfd27f.jpg"));
        QCOMPARE(p.jsonPath,
                 QStringLiteral("/dest/gelbooru.com/"
                                "gelboorucom_516e1f01a05ea895b33316bd28bfd27f.json"));
    }

    void md5PathsOnDiskReturnsRealFile() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        QDir(tmp.path()).mkpath(QStringLiteral("gelbooru.com"));
        const QString md5 = QStringLiteral("516e1f01a05ea895b33316bd28bfd27f");
        const QString media = tmp.filePath(
            QStringLiteral("gelbooru.com/gelboorucom_") + md5 +
            QStringLiteral(".jpg"));
        QFile f(media);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("x");
        f.close();
        const QHash<QString, QString> paths =
            LosnImport::md5PathsOnDisk(tmp.path());
        QCOMPARE(paths.value(md5), media);
        QCOMPARE(LosnImport::md5sOnDisk(tmp.path()), QSet<QString>{md5});
    }

    void syncOnlyTouchesBackupSites() {
        // Sync is opt-in and scoped: a Gelbooru backup may drop stale
        // Gelbooru favourites but never a stray or a Danbooru row.
        Post gel, gelKeep, dan, stray;
        gel.siteName = QStringLiteral("Gelbooru");
        gel.id = 1;
        gelKeep.siteName = QStringLiteral("Gelbooru");
        gelKeep.id = 2;
        dan.siteName = QStringLiteral("Danbooru");
        dan.id = 3;
        stray.siteName = QStringLiteral("_Stray");
        stray.id = 0;
        const QSet<QString> keep{QStringLiteral("Gelbooru#2")};
        const QSet<QString> sites{QStringLiteral("Gelbooru")};
        const QVector<LosnImport::FavKey> stale =
            LosnImport::staleFavourites({gel, gelKeep, dan, stray}, keep,
                                        sites);
        QCOMPARE(stale.size(), 1);
        QCOMPARE(stale.first().site, QStringLiteral("Gelbooru"));
        QCOMPARE(stale.first().id, 1LL);
        // No sync sites -> nothing can be removed.
        QVERIFY(LosnImport::staleFavourites({gel}, {}, {}).isEmpty());
    }

    void pathsForRowPrefersSidecarMd5() {
        // Refreshed rows carry a real md5 in the sidecar while the row
        // itself has none: naming must use it, not the losn fallback.
        LosnRow row;
        row.rowId = 9;
        row.siteLabel = QStringLiteral("Gelbooru");
        row.fileUrl = QStringLiteral("https://example.com/f.png");
        QJsonObject sidecar;
        sidecar[QStringLiteral("id")] = 42;
        sidecar[QStringLiteral("md5")] =
            QStringLiteral("d41d8cd98f00b204e9800998ecf8427e");
        const LosnImport::DownloadPaths p =
            LosnImport::pathsForRow(row, sidecar, QStringLiteral("/d"));
        QCOMPARE(p.fileName,
                 QStringLiteral("Gelbooru_d41d8cd98f00b204e9800998ecf8427e.png"));
        // Szurubooru-style checksum key works too.
        QJsonObject szuru;
        szuru[QStringLiteral("id")] = 43;
        szuru[QStringLiteral("checksumMD5")] =
            QStringLiteral("900150983cd24fb0d6963f7d28e17f72");
        const LosnImport::DownloadPaths q =
            LosnImport::pathsForRow(row, szuru, QStringLiteral("/d"));
        QCOMPARE(q.fileName,
                 QStringLiteral("Gelbooru_900150983cd24fb0d6963f7d28e17f72.png"));
        // No md5 anywhere: same fallback as targetPaths.
        const LosnImport::DownloadPaths r =
            LosnImport::pathsForRow(row, QJsonObject(), QStringLiteral("/d"));
        QCOMPARE(r.fileName, QStringLiteral("Gelbooru_losn9.png"));
    }

    void mergeRowFallsBackToFileHost() {
        // Merge-tab rows often lack a post URL: the file (CDN) host still
        // resolves the booru so they file under the right host folder.
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                    QStringLiteral("t_merge"));
        db.setDatabaseName(tmp.filePath(QStringLiteral("store.db")));
        QVERIFY(db.open());
        QSqlQuery q(db);
        QVERIFY(q.exec(QStringLiteral("CREATE TABLE Tag (id INTEGER PRIMARY KEY, name TEXT)")));
        QVERIFY(q.exec(QStringLiteral("CREATE TABLE ImageTag (tagID INTEGER, booruItemID INTEGER)")));
        QVERIFY(q.exec(QStringLiteral("CREATE TABLE BooruItem (id INTEGER PRIMARY KEY,"
                                      " thumbnailURL TEXT, sampleURL TEXT, fileURL TEXT,"
                                      " postURL TEXT, mediaType TEXT,"
                                      " isSnatched INTEGER, isFavourite INTEGER)")));
        QVERIFY(q.exec(QStringLiteral(
            "INSERT INTO BooruItem VALUES (20, 't', 's',"
            " 'https://cdn.donmai.us/original/57/81/57819e03cd64d511be7ae2644e2e.jpg',"
            " '', 'image', 1, 0)")));
        db.close();
        QSqlDatabase::removeDatabase(QStringLiteral("t_merge"));
        QString err;
        const QList<LosnRow> rows =
            LosnImport::readBackup(tmp.filePath(QStringLiteral("store.db")), err);
        QVERIFY(err.isEmpty());
        QCOMPARE(rows.size(), 1);
        QCOMPARE(rows.first().siteKey, QStringLiteral("danbooru"));
        QCOMPARE(rows.first().siteLabel, QStringLiteral("Danbooru"));
    }

    void md5sOnDiskFindsCrossFolderHits() {
        // Re-imports skip by content hash anywhere under destDir, even when
        // an earlier run filed the file under a stale label.
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        QDir(tmp.path()).mkpath(QStringLiteral("Merge"));
        QDir(tmp.path()).mkpath(QStringLiteral("Danbooru"));
        const QString md5 = QStringLiteral("4f0a3d37c18e61c8550ab82b94ef06a2");
        QFile f(tmp.filePath(QStringLiteral("Merge/Merge_") + md5 + QStringLiteral(".jpg")));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("x");
        f.close();
        QFile g(tmp.filePath(QStringLiteral("Danbooru/other.txt")));
        QVERIFY(g.open(QIODevice::WriteOnly));
        g.write("x");
        g.close();
        const QSet<QString> found = LosnImport::md5sOnDisk(tmp.path());
        QCOMPARE(found.size(), 1);
        QVERIFY(found.contains(md5));
        QVERIFY(LosnImport::md5sOnDisk(tmp.filePath(QStringLiteral("nope"))).isEmpty());
    }

    void pathsForRowSecurityAndMd5Url() {
        // src/library/losnimport.cpp:108-130 (pathsForRow), :84-92 (md5FromFileUrl)
        LosnRow row;
        row.rowId = 7;
        row.siteLabel = QStringLiteral("Gelbooru");
        row.fileUrl = QStringLiteral("https://example.com/f.png");
        // Rogue/traversal or non-hex md5 in sidecar must fall back to losn name.
        QJsonObject bad;
        bad[QStringLiteral("md5")] = QStringLiteral("../../etc/passwd");
        QCOMPARE(LosnImport::pathsForRow(row, bad, QStringLiteral("/d")).fileName,
                 QStringLiteral("Gelbooru_losn7.png"));
        bad[QStringLiteral("md5")] = QStringLiteral("not-a-hash");
        QCOMPARE(LosnImport::pathsForRow(row, bad, QStringLiteral("/d")).fileName,
                 QStringLiteral("Gelbooru_losn7.png"));
        // pathsForRow normalizes smd (trim+lower) before hex check.
        QJsonObject upper;
        upper[QStringLiteral("md5")] = QStringLiteral("  D41D8CD98F00B204E9800998ECF8427E  ");
        QCOMPARE(LosnImport::pathsForRow(row, upper, QStringLiteral("/d")).fileName,
                 QStringLiteral("Gelbooru_d41d8cd98f00b204e9800998ecf8427e.png"));

        // md5FromFileUrl: hex32 is case-insensitive and normalizes to
        // lowercase (the disk index is lowercase too).
        QCOMPARE(LosnImport::md5FromFileUrl(
                     QStringLiteral("https://x/y/d41d8cd98f00b204e9800998ecf8427e.jpg")),
                 QStringLiteral("d41d8cd98f00b204e9800998ecf8427e"));
        QCOMPARE(LosnImport::md5FromFileUrl(
                     QStringLiteral("https://x/y/D41D8CD98F00B204E9800998ECF8427E.jpg")),
                 QStringLiteral("d41d8cd98f00b204e9800998ecf8427e"));
        QVERIFY(LosnImport::md5FromFileUrl(QStringLiteral("https://x/y/nope.jpg"))
                    .isEmpty());
    }

    void losnGaps() {
        // src/library/losnimport.cpp
        // siteForHost: known host
        QString key, label;
        LosnImport::siteForHost(QStringLiteral("gelbooru.com"), key, label);
        QCOMPARE(key, QStringLiteral("gelbooru"));
        QVERIFY(!label.isEmpty());
        // siteForHost: unknown host derives label
        LosnImport::siteForHost(QStringLiteral("unknown.example"), key, label);
        QVERIFY(!key.isEmpty() || !label.isEmpty());
        // idFromPostUrl: various forms
        QCOMPARE(LosnImport::idFromPostUrl(QStringLiteral("https://x/posts/123")), 123LL);
        QCOMPARE(LosnImport::idFromPostUrl(QStringLiteral("https://x/post/show/456")), 456LL);
        QCOMPARE(LosnImport::idFromPostUrl(QStringLiteral("https://x/?id=789")), 789LL);
        QCOMPARE(LosnImport::idFromPostUrl(QStringLiteral("https://x/no-id")), 0LL);
        // md5sOnDisk: empty/nonexistent -> empty
        QVERIFY(LosnImport::md5sOnDisk(QString()).isEmpty());
        QVERIFY(LosnImport::md5sOnDisk(QStringLiteral("/nonexistent")).isEmpty());
        // diffNewFiles: new vs already-present by md5 stem.
        const QString oldMd5 = QStringLiteral("4f0a3d37c18e61c8550ab82b94ef06a2");
        const QString newMd5 = QStringLiteral("d41d8cd98f00b204e9800998ecf8427e");
        const LosnImport::SummaryDiff diff = LosnImport::diffNewFiles(
            {oldMd5},
            {{QStringLiteral("https://x/a.jpg"),
              QStringLiteral("/d/Site_") + oldMd5 + QStringLiteral(".jpg")},
             {QStringLiteral("https://x/b.jpg"),
              QStringLiteral("/d/Site_") + newMd5 + QStringLiteral(".jpg")},
             {QStringLiteral("https://x/c.jpg"),
              QStringLiteral("/d/Site_losn9.jpg")}});
        QCOMPARE(diff.newFiles.size(), 2);
        QVERIFY(diff.newFiles.contains(QStringLiteral("Site_") + newMd5 + QStringLiteral(".jpg")));
        QVERIFY(diff.newFiles.contains(QStringLiteral("Site_losn9.jpg")));
        QCOMPARE(diff.alreadyPresent.size(), 1);
        QVERIFY(diff.alreadyPresent.contains(QStringLiteral("Site_") + oldMd5 + QStringLiteral(".jpg")));
    }

    void readSyntheticBackup() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        makeBackup(tmp.filePath(QStringLiteral("store.db")));
        QString err;
        const QList<LosnRow> rows =
            LosnImport::readBackup(tmp.filePath(QStringLiteral("store.db")), err);
        QVERIFY(err.isEmpty());
        QCOMPARE(rows.size(), 2);
        const LosnRow &gel = rows.at(0);
        QVERIFY(gel.snatched && gel.favourite);
        QCOMPARE(gel.siteKey, QStringLiteral("gelbooru"));
        QCOMPARE(gel.siteLabel, QStringLiteral("Gelbooru"));
        QCOMPARE(gel.host, QStringLiteral("gelbooru.com"));
        QCOMPARE(gel.postId, 7103104LL);
        QCOMPARE(gel.md5, QStringLiteral("516e1f01a05ea895b33316bd28bfd27f"));
        QCOMPARE(gel.tags, QStringList({QStringLiteral("1girl"), QStringLiteral("sky")}));
        const LosnRow &dan = rows.at(1);
        QVERIFY(!dan.snatched && dan.favourite);
        QCOMPARE(dan.siteKey, QStringLiteral("danbooru"));
        QCOMPARE(dan.postId, 5LL);
        QVERIFY(dan.md5.isEmpty()); // non-md5 URL: no md5 stem
    }

    void readMissingBackupFails() {
        QString err;
        const QList<LosnRow> rows =
            LosnImport::readBackup(QStringLiteral("/nonexistent/store.db"), err);
        QVERIFY(rows.isEmpty());
        QVERIFY(!err.isEmpty());
    }
};

int main(int argc, char *argv[]) {
    // QCoreApplication (not appless): QSqlDatabase and friends
    // require an application instance. Still no display needed.
    QCoreApplication app(argc, argv);
    TestLosnImport tc;
    return QTest::qExec(&tc, argc, argv);
}
#include "test_losnimport.moc"
