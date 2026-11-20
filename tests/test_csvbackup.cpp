// Portable library CSV backup: writer/parser shape, aliases, quoting,
// tolerance and refusal of foreign files. Pure (no network, no DB).
#include "csvbackup.h"

#include <QCoreApplication>
#include <QtTest>

class TestCsvBackup : public QObject {
    Q_OBJECT
private slots:
    void headerIsStable() {
        const QByteArray out = CsvBackup::write({});
        QVERIFY(out.startsWith(
            "post_url,file_url,downloaded,favourited,date_added,site,"
            "post_id,md5,rating,ext,width,height,tags,local_file\n"));
        QCOMPARE(out.count('\n'), 1);
    }

    void roundTrip() {
        CsvBackup::Row a;
        a.postUrl = QStringLiteral("https://gelbooru.com/index.php?page=post&s=view&id=42");
        a.fileUrl = QStringLiteral("https://img3.gelbooru.com/images/aa/bb/aabb.jpg");
        a.downloaded = true;
        a.favourited = true;
        a.dateAdded = QStringLiteral("2026-11-20T05:13:00Z");
        a.site = QStringLiteral("Gelbooru");
        a.postId = 42;
        a.md5 = QStringLiteral("aabbccddeeff00112233445566778899");
        a.rating = QStringLiteral("general");
        a.ext = QStringLiteral("jpg");
        a.width = 1200;
        a.height = 800;
        a.tags = QStringLiteral("1girl, solo, \"quoted\", multi\nline");
        a.localFile = QStringLiteral("gelbooru.com/gelbooru_42.jpg");

        CsvBackup::Row b;
        b.postUrl = QStringLiteral("https://danbooru.donmai.us/posts/7");
        b.downloaded = false;
        b.favourited = true;
        b.site = QStringLiteral("Danbooru");
        b.postId = 7;

        QString error = QStringLiteral("stale");
        const QVector<CsvBackup::Row> back =
            CsvBackup::parse(CsvBackup::write({a, b}), &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(back.size(), 2);
        QCOMPARE(back.at(0).postUrl, a.postUrl);
        QCOMPARE(back.at(0).fileUrl, a.fileUrl);
        QVERIFY(back.at(0).downloaded);
        QVERIFY(back.at(0).favourited);
        QCOMPARE(back.at(0).dateAdded, a.dateAdded);
        QCOMPARE(back.at(0).site, a.site);
        QCOMPARE(back.at(0).postId, 42);
        QCOMPARE(back.at(0).md5, a.md5);
        QCOMPARE(back.at(0).rating, a.rating);
        QCOMPARE(back.at(0).ext, a.ext);
        QCOMPARE(back.at(0).width, 1200);
        QCOMPARE(back.at(0).height, 800);
        QCOMPARE(back.at(0).tags, a.tags);
        QCOMPARE(back.at(0).localFile, a.localFile);
        QCOMPARE(back.at(1).postId, 7);
        QVERIFY(!back.at(1).downloaded);
        QVERIFY(back.at(1).favourited);
    }

    void readmeAliasesAndOrder() {
        // A hand-written/edited file: reordered columns, README-style
        // names, 0/1 booleans, CRLF and a BOM.
        const QByteArray data =
            "\xEF\xBB\xBF" "favorited,postURL,dowloaded,fileURL,added,source,id,"
            "width,tags,md5\r\n"
            "yes,https://danbooru.donmai.us/posts/9,0,"
            "https://cdn.donmai.us/a/b/c.jpg,2026-01-02T03:04:05Z,Danbooru,9,"
            "600,\"a b\",00112233445566778899aabbccddeeff\r\n";
        QString error = QStringLiteral("stale");
        const auto rows = CsvBackup::parse(data, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(rows.size(), 1);
        QVERIFY(rows.at(0).favourited);
        QVERIFY(!rows.at(0).downloaded);
        QCOMPARE(rows.at(0).postId, 9);
        QCOMPARE(rows.at(0).site, QStringLiteral("Danbooru"));
        QCOMPARE(rows.at(0).width, 600);
        QCOMPARE(rows.at(0).dateAdded,
                 QStringLiteral("2026-01-02T03:04:05Z"));
        QCOMPARE(rows.at(0).md5,
                 QStringLiteral("00112233445566778899aabbccddeeff"));
    }

    void rejectsForeignFiles() {
        QString error;
        QVERIFY(CsvBackup::parse("name,age\nbob,3\n", &error).isEmpty());
        QVERIFY(!error.isEmpty());
        error.clear();
        QVERIFY(CsvBackup::parse({}, &error).isEmpty());
        QVERIFY(!error.isEmpty());
        error = QStringLiteral("stale");
        QVERIFY(CsvBackup::parse(
                    "post_url,file_url\n"
                    "https://x/posts/1,\n"
                    "https://x/posts/2,\n",
                    &error)
                    .size() == 2);
        QVERIFY(error.isEmpty());
    }

    void rowsNeedSomeIdentity() {
        // A line with no URL and no local path is skipped, not imported as
        // a nameless entry.
        const auto rows = CsvBackup::parse(
            "post_url,file_url,site\n"
            ",,Gelbooru\n"
            "https://x/posts/3,,Gelbooru\n");
        QCOMPARE(rows.size(), 1);
        QCOMPARE(rows.at(0).postId, 0);
    }

    void boolTolerance() {
        bool ok = false;
        QVERIFY(CsvBackup::parseBool(QStringLiteral(" TRUE "), &ok));
        QVERIFY(ok);
        QVERIFY(CsvBackup::parseBool(QStringLiteral("1"), &ok));
        QVERIFY(ok);
        QVERIFY(CsvBackup::parseBool(QStringLiteral("yes"), &ok));
        QVERIFY(ok);
        QVERIFY(!CsvBackup::parseBool(QStringLiteral("0"), &ok));
        QVERIFY(ok);
        QVERIFY(!CsvBackup::parseBool(QStringLiteral("false"), &ok));
        QVERIFY(ok);
        QVERIFY(!CsvBackup::parseBool(QString(), &ok));
        QVERIFY(ok);
        QVERIFY(!CsvBackup::parseBool(QStringLiteral("maybe"), &ok));
        QVERIFY(!ok);
    }

    void unterminatedQuoteIsRejected() {
        QString error;
        QVERIFY(CsvBackup::parse(
                    "post_url,file_url\n\"https://x/posts/1,\n", &error)
                    .isEmpty());
        QVERIFY(!error.isEmpty());
    }
};

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    TestCsvBackup tc;
    return QTest::qExec(&tc, argc, argv);
}
#include "test_csvbackup.moc"
