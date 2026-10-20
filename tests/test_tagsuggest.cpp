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
        QCOMPARE(TagSuggester::parse(BooruEngine::Danbooru, body).size(), 24);
    }

    void gelbooruXmlFallback() {
        // Older builds (Safebooru) answer the tag endpoint with XML:
        // same attributes, must parse instead of yielding nothing.
        const QByteArray body =
            R"(<?xml version="1.0" encoding="UTF-8"?><tags type="array"><tag type="0" count="3" name="sky-raft" id="59190"/><tag type="1" count="1" name="skyqidog" id="59541"/><tag count="1" name="" id="1"/></tags>)";
        const auto out = TagSuggester::parse(BooruEngine::Gelbooru, body);
        QCOMPARE(out.size(), 2);
        QCOMPARE(out.at(0).tag, QStringLiteral("sky-raft"));
        QCOMPARE(out.at(0).bucket, QStringLiteral("general"));
        QCOMPARE(out.at(0).count, 3);
        QCOMPARE(out.at(1).bucket, QStringLiteral("artist"));
    }

    void rankOrdersExactFirst() {
        // API order is count-only: ranking lifts the exact tag above
        // higher-count prefix matches, keeps prefix by count, and sinks
        // non-matches last (the popup prefix-filters them away anyway).
        auto sug = [](const char *tag, int count) {
            TagSuggestion s;
            s.tag = QString::fromLatin1(tag);
            s.count = count;
            return s;
        };
        const QVector<TagSuggestion> in = {
            sug("skyline", 9000), sug("SKY", 30), sug("unrelated", 99999),
            sug("skydive", 400)};
        const auto out = TagSuggester::rank(QStringLiteral("sky"), in);
        QCOMPARE(out.size(), 4);
        QCOMPARE(out.at(0).tag, QStringLiteral("SKY")); // exact, case-insensitive
        QCOMPARE(out.at(1).tag, QStringLiteral("skyline")); // prefix by count
        QCOMPARE(out.at(2).tag, QStringLiteral("skydive"));
        QCOMPARE(out.at(3).tag, QStringLiteral("unrelated")); // sunk
        // Longer tags win count ties over short noise.
        const QVector<TagSuggestion> tied = {sug("sob", 5), sug("sober_long", 5)};
        QCOMPARE(TagSuggester::rank(QStringLiteral("so"), tied).at(0).tag,
                 QStringLiteral("sober_long"));
        QVERIFY(TagSuggester::rank(QStringLiteral("so"), {}).isEmpty());
    }

    void rankFavTagsFirst() {
        // Starred prefix matches outrank higher-count plain ones; the
        // exact tag still wins over a starred non-exact tag.
        auto sug = [](const char *tag, int count) {
            TagSuggestion s;
            s.tag = QString::fromLatin1(tag);
            s.count = count;
            return s;
        };
        const QSet<QString> fav = {QStringLiteral("skydive")};
        const QVector<TagSuggestion> in = {
            sug("skyline", 9000), sug("skydive", 400), sug("sky", 30)};
        const auto out = TagSuggester::rank(QStringLiteral("sky"), in, fav);
        QCOMPARE(out.at(0).tag, QStringLiteral("sky")); // exact still first
        QCOMPARE(out.at(1).tag, QStringLiteral("skydive")); // starred prefix
        QCOMPARE(out.at(2).tag, QStringLiteral("skyline"));
        // No favs: previous order stands.
        const auto plain = TagSuggester::rank(QStringLiteral("sky"), in);
        QCOMPARE(plain.at(1).tag, QStringLiteral("skyline"));
    }

    void mergeKeepsLocalsFirst() {
        // Remote extras append below locals; dupes collapse (local wins);
        // an empty remote never blanks good local answers.
        auto sug = [](const char *tag, const char *bucket, int count) {
            TagSuggestion s;
            s.tag = QString::fromLatin1(tag);
            s.bucket = QString::fromLatin1(bucket);
            s.count = count;
            return s;
        };
        const QVector<TagSuggestion> local = {
            sug("sky", "general", 30), sug("skirt", "general", 5)};
        const QVector<TagSuggestion> remote = {
            sug("SKY", "general", 999), // dupe: local row wins
            sug("skyline", "general", 400)};
        const auto merged = TagSuggester::merge(local, remote);
        QCOMPARE(merged.size(), 3);
        QCOMPARE(merged.at(0).tag, QStringLiteral("sky"));
        QCOMPARE(merged.at(0).count, 30); // local count kept, not remote's
        QCOMPARE(merged.at(2).tag, QStringLiteral("skyline"));
        // Empty remote keeps locals untouched.
        QCOMPARE(TagSuggester::merge(local, {}).size(), 2);
        QVERIFY(TagSuggester::merge({}, {}).isEmpty());
        // Cap honored, locals first.
        QVector<TagSuggestion> many;
        for (int i = 0; i < 20; ++i)
            many.append(sug(QByteArray("l" + QByteArray::number(i)).constData(),
                            "general", i));
        QCOMPARE(TagSuggester::merge(many, remote).size(), 12);
    }
};

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    TestTagSuggest tc;
    return QTest::qExec(&tc, argc, argv);
}
#include "test_tagsuggest.moc"
