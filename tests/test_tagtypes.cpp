// Tag-category classification: danbooru numbering, Safebooru XML and
// yandere JSON response parsers, persistent tag->type store.
#include "tagtypes.h"

#include <QTemporaryDir>
#include <QCoreApplication>
#include <QtTest>

class TestTagTypes : public QObject {
    Q_OBJECT
private slots:
    void bucketRoundTrip() {
        QCOMPARE(TagTypes::bucketForType(0), QStringLiteral("general"));
        QCOMPARE(TagTypes::bucketForType(1), QStringLiteral("artist"));
        QCOMPARE(TagTypes::bucketForType(3), QStringLiteral("copyright"));
        QCOMPARE(TagTypes::bucketForType(4), QStringLiteral("character"));
        QCOMPARE(TagTypes::bucketForType(5), QStringLiteral("meta"));
        QCOMPARE(TagTypes::bucketForType(6), QStringLiteral("general")); // deprecated
        QCOMPARE(TagTypes::bucketForType(-1), QStringLiteral("general"));
        QCOMPARE(TagTypes::bucketForType(999), QStringLiteral("general"));
        QCOMPARE(TagTypes::typeForBucket(QStringLiteral("artist")), 1);
        QCOMPARE(TagTypes::typeForBucket(QStringLiteral("copyright")), 3);
        QCOMPARE(TagTypes::typeForBucket(QStringLiteral("character")), 4);
        QCOMPARE(TagTypes::typeForBucket(QStringLiteral("meta")), 5);
        QCOMPARE(TagTypes::typeForBucket(QStringLiteral("general")), 0);
        QCOMPARE(TagTypes::typeForBucket(QStringLiteral("bogus")), 0);
    }

    void safebooruExactMatch() {
        const QByteArray xml =
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
            "<tags type=\"array\">"
            "<tag id=\"1\" name=\"hatsune_miku\" type=\"4\" count=\"100\"/>"
            "<tag id=\"2\" name=\"hatsune_miku_cosplay\" type=\"0\" count=\"3\"/>"
            "</tags>";
        QCOMPARE(TagTypes::parseSafebooruXml(xml, QStringLiteral("hatsune_miku")), 4);
        // Substring is not enough: exact match only.
        QCOMPARE(TagTypes::parseSafebooruXml(xml, QStringLiteral("hatsune")),
                 TagTypes::kUnknownTag);
        QCOMPARE(TagTypes::parseSafebooruXml(xml, QStringLiteral("missing")),
                 TagTypes::kUnknownTag);
    }

    void safebooruErrorsAreTransient() {
        QCOMPARE(TagTypes::parseSafebooruXml("not xml at all",
                                             QStringLiteral("x")),
                 TagTypes::kTransientFailure);
        QCOMPARE(TagTypes::parseSafebooruXml("", QStringLiteral("x")),
                 TagTypes::kTransientFailure);
        const QByteArray badType =
            "<tags><tag name=\"x\" type=\"abc\"/></tags>";
        QCOMPARE(TagTypes::parseSafebooruXml(badType, QStringLiteral("x")),
                 TagTypes::kTransientFailure);
    }

    void yandereExactMatch() {
        const QByteArray json =
            "[{\"id\":1,\"name\":\"vocaloid\",\"type\":3,\"count\":50},"
            " {\"id\":2,\"name\":\"vocaloid_cosplay\",\"type\":0,\"count\":2}]";
        QCOMPARE(TagTypes::parseYandereJson(json, QStringLiteral("vocaloid")), 3);
        QCOMPARE(TagTypes::parseYandereJson(json, QStringLiteral("vocal")),
                 TagTypes::kUnknownTag);
        QCOMPARE(TagTypes::parseYandereJson(json, QStringLiteral("missing")),
                 TagTypes::kUnknownTag);
    }

    void yandereErrorsAreTransient() {
        QCOMPARE(TagTypes::parseYandereJson("garbage", QStringLiteral("x")),
                 TagTypes::kTransientFailure);
        QCOMPARE(TagTypes::parseYandereJson("{\"name\":\"x\"}", QStringLiteral("x")),
                 TagTypes::kTransientFailure); // not an array
    }

    void danbooruExactMatch() {
        const QByteArray json =
            "[{\"id\":385430,\"name\":\"hatsune_miku\",\"category\":4},"
            " {\"id\":9,\"name\":\"hatsune_miku_cosplay\",\"category\":0}]";
        QCOMPARE(TagTypes::parseDanbooruTagsJson(json, QStringLiteral("hatsune_miku")), 4);
        // Substring is not enough: exact match only.
        QCOMPARE(TagTypes::parseDanbooruTagsJson(json, QStringLiteral("hatsune")),
                 TagTypes::kUnknownTag);
        QCOMPARE(TagTypes::parseDanbooruTagsJson(json, QStringLiteral("missing")),
                 TagTypes::kUnknownTag);
        QCOMPARE(TagTypes::parseDanbooruTagsJson("[]", QStringLiteral("x")),
                 TagTypes::kUnknownTag); // well-formed, empty
    }

    void danbooruErrorsAreTransient() {
        QCOMPARE(TagTypes::parseDanbooruTagsJson("garbage", QStringLiteral("x")),
                 TagTypes::kTransientFailure);
        QCOMPARE(TagTypes::parseDanbooruTagsJson("{\"name\":\"x\"}", QStringLiteral("x")),
                 TagTypes::kTransientFailure); // not an array
    }

    void cachePutGetBatch() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        TagTypeCache cache;
        QVERIFY(cache.open(tmp.filePath(QStringLiteral("tags.db"))));
        int type = 12345;
        QVERIFY(!cache.get(QStringLiteral("nope"), type)); // miss
        cache.put(QStringLiteral("hatsune_miku"), 4);
        QVERIFY(cache.get(QStringLiteral("hatsune_miku"), type));
        QCOMPARE(type, 4);
        cache.putBatch({{"a", 1}, {"b", TagTypes::kUnknownTag}});
        QVERIFY(cache.get(QStringLiteral("a"), type));
        QCOMPARE(type, 1);
        QVERIFY(cache.get(QStringLiteral("b"), type)); // misses persist too
        QCOMPARE(type, TagTypes::kUnknownTag);
        cache.close();
        // Verdicts survive reopening.
        TagTypeCache reopened;
        QVERIFY(reopened.open(tmp.filePath(QStringLiteral("tags.db"))));
        QVERIFY(reopened.get(QStringLiteral("hatsune_miku"), type));
        QCOMPARE(type, 4);
    }

    void seedParseAndImport() {
        const QHash<QString, int> m = TagTypes::parseTagSeed(
            QByteArray("hatsune_miku\t4\n"
                       "# comment\n"
                       "\n"
                       "sky\t0\n"
                       "bogus-line\n"
                       "sky\t1\n" // later wins
                       "x\tnot-an-int\n"));
        QCOMPARE(m.size(), 2);
        QCOMPARE(m.value(QStringLiteral("hatsune_miku")), 4);
        QCOMPARE(m.value(QStringLiteral("sky")), 1);
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        TagTypeCache cache;
        QVERIFY(cache.open(tmp.filePath(QStringLiteral("tags.db"))));
        QCOMPARE(cache.importSeed(m), 2);
        int type = -99;
        QVERIFY(cache.get(QStringLiteral("sky"), type));
        QCOMPARE(type, 1);
        // Re-import never clobbers: fetched verdicts win over the seed.
        cache.put(QStringLiteral("sky"), 4);
        QCOMPARE(cache.importSeed(m), 0);
        QVERIFY(cache.get(QStringLiteral("sky"), type));
        QCOMPARE(type, 4);
    }

    void overrideLinesNormalize() {
        const QMap<QString, QString> m = TagTypes::normalizeTagOverrides(
            {QStringLiteral("  Lucky_Star = Copyright "),
             QStringLiteral("hatsune_miku=character"),
             QStringLiteral("no-equals-sign"), QStringLiteral("=artist"),
             QStringLiteral("x=bogus-bucket"), QString(),
             QStringLiteral("hatsune_miku=meta")}); // later wins
        QCOMPARE(m.size(), 2);
        QCOMPARE(m.value(QStringLiteral("lucky_star")), QStringLiteral("copyright"));
        QCOMPARE(m.value(QStringLiteral("hatsune_miku")), QStringLiteral("meta"));
        QVERIFY(TagTypes::isValidBucket(QStringLiteral("general")));
        QVERIFY(!TagTypes::isValidBucket(QStringLiteral("bogus")));
        // Serialization round-trips through the same parser.
        QCOMPARE(TagTypes::normalizeTagOverrides(
                     TagTypes::serializeTagOverrides(m)),
                 m);
    }
};

int main(int argc, char *argv[]) {
    // QCoreApplication (not appless): QSqlDatabase and friends
    // require an application instance. Still no display needed.
    QCoreApplication app(argc, argv);
    TestTagTypes tc;
    return QTest::qExec(&tc, argc, argv);
}
#include "test_tagtypes.moc"
