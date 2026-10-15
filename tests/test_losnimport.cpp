// LoliSnatcher backup import: host/site mapping, post-id + md5 URL
// parsing, gallery-dl-shaped fallback sidecars, synthetic store.db reads.
#include "losnimport.h"
#include "metaparser.h"

#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QCoreApplication>
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
        QVERIFY(LosnImport::md5FromFileUrl(QStringLiteral("https://example.com/x.png")).isEmpty());
        QVERIFY(LosnImport::md5FromFileUrl(QString()).isEmpty());
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
