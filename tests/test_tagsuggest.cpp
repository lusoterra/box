// Remote tag autocomplete response parsers: per-engine shapes, buckets,
// caps, garbage tolerance.
#include "tagsuggest.h"

#include <QCoreApplication>
#include <QtTest>

class TestTagSuggest : public QObject {
    Q_OBJECT
private slots:
    void danbooru() {
        const QByteArray body =
            R"([{"name":"hatsune_miku","category":4,"post_count":500},
                {"name":"vocaloid","category":3,"post_count":300},
                {"name":"","category":1,"post_count":1}])";
        const auto out = TagSuggester::parse(BooruEngine::Danbooru, body);
        QCOMPARE(out.size(), 2);
        QCOMPARE(out.at(0).tag, QStringLiteral("hatsune_miku"));
        QCOMPARE(out.at(0).bucket, QStringLiteral("character"));
        QCOMPARE(out.at(0).count, 500);
        QCOMPARE(out.at(1).bucket, QStringLiteral("copyright"));
    }

    void gelbooru() {
        const QByteArray body =
            R"({"tag":[{"id":1,"name":"an_artist","type":1,"count":50}]})";
        const auto out = TagSuggester::parse(BooruEngine::Gelbooru, body);
        QCOMPARE(out.size(), 1);
        QCOMPARE(out.at(0).tag, QStringLiteral("an_artist"));
        QCOMPARE(out.at(0).bucket, QStringLiteral("artist"));
        QCOMPARE(out.at(0).count, 50);
        // Bare arrays tolerated too.
        const auto bare = TagSuggester::parse(
            BooruEngine::Gelbooru, QByteArray(R"([{"name":"x","type":0,"count":2}])"));
        QCOMPARE(bare.size(), 1);
        QCOMPARE(bare.at(0).bucket, QStringLiteral("general"));
    }

    void moebooru() {
        const QByteArray body = R"([{"name":"twintails","type":0,"count":9}])";
        const auto out = TagSuggester::parse(BooruEngine::Moebooru, body);
        QCOMPARE(out.size(), 1);
        QCOMPARE(out.at(0).bucket, QStringLiteral("general"));
    }

    void szuru() {
        const QByteArray body =
            R"({"results":[{"names":["some_series"],"category":"series","usages":7},
                {"names":[],"category":"artist","usages":1}]})";
        const auto out = TagSuggester::parse(BooruEngine::Szurubooru, body);
        QCOMPARE(out.size(), 1); // empty-names entry skipped
        QCOMPARE(out.at(0).tag, QStringLiteral("some_series"));
        QCOMPARE(out.at(0).bucket, QStringLiteral("copyright"));
        QCOMPARE(out.at(0).count, 7);
    }

    void garbageAndShimmie() {
        QVERIFY(TagSuggester::parse(BooruEngine::Danbooru, QByteArray("nope")).isEmpty());
        QVERIFY(TagSuggester::parse(BooruEngine::Gelbooru, QByteArray("{}")).isEmpty());
        QVERIFY(TagSuggester::parse(BooruEngine::Szurubooru, QByteArray("[]")).isEmpty());
        QVERIFY(TagSuggester::parse(BooruEngine::Shimmie, QByteArray("[{}]")).isEmpty());
    }

    void capped() {
        QByteArray body = "[";
        for (int i = 0; i < 30; ++i)
            body += QString(R"({"name":"t%1","category":0,"post_count":%1},)").arg(i).toUtf8();
        body.chop(1);
        body += "]";
        QCOMPARE(TagSuggester::parse(BooruEngine::Danbooru, body).size(), 12);
    }
};

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    TestTagSuggest tc;
    return QTest::qExec(&tc, argc, argv);
}
#include "test_tagsuggest.moc"
