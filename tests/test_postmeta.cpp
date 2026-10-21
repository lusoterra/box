// PostMeta notes/comments endpoints per engine + note parsing.
// Pure URL/shape tests (no network).
#include "postmeta.h"

#include <QCoreApplication>
#include <QUrlQuery>
#include <QtTest>

namespace {
QString queryItem(const QUrl &url, const char *key) {
    return QUrlQuery(url).queryItemValue(QString::fromLatin1(key));
}
const QString kDan = QStringLiteral("https://danbooru.donmai.us");
const QString kGel = QStringLiteral("https://gelbooru.com");
const QString kSfw = QStringLiteral("https://safebooru.org");
const QString kMoe = QStringLiteral("https://yande.re");
} // namespace

class TestPostMeta : public QObject {
    Q_OBJECT
private slots:
    void notesUrls() {
        QCOMPARE(PostMeta::notesUrl(BooruEngine::Danbooru, kDan, 42).toString(),
                 kDan + QStringLiteral("/notes.json?search[post_id]=42&limit=100"));
        const QUrl gel =
            PostMeta::notesUrl(BooruEngine::Gelbooru, kGel, 42, {}, {});
        QCOMPARE(queryItem(gel, "s"), QStringLiteral("note"));
        QCOMPARE(queryItem(gel, "post_id"), QStringLiteral("42"));
        // Safebooru rides the Gelbooru dapi shape (not Moebooru's).
        const QUrl sfw =
            PostMeta::notesUrl(BooruEngine::Safebooru, kSfw, 42, {}, {});
        QCOMPARE(queryItem(sfw, "s"), QStringLiteral("note"));
        QVERIFY(sfw.toString().startsWith(kSfw));
        QCOMPARE(PostMeta::notesUrl(BooruEngine::Moebooru, kMoe, 42).toString(),
                 kMoe + QStringLiteral("/note.json?post_id=42"));
        // No separate notes endpoint here.
        QVERIFY(PostMeta::notesUrl(BooruEngine::Szurubooru, kDan, 1).isEmpty());
        QVERIFY(PostMeta::notesUrl(BooruEngine::Shimmie, kDan, 1).isEmpty());
        QVERIFY(PostMeta::notesUrl(BooruEngine::Hybooru, kDan, 1).isEmpty());
    }

    void commentsUrls() {
        QVERIFY(PostMeta::commentsUrl(BooruEngine::Danbooru, kDan, 7)
                    .toString()
                    .contains(QStringLiteral("/comments.json")));
        const QUrl sfw =
            PostMeta::commentsUrl(BooruEngine::Safebooru, kSfw, 7, {}, {});
        QCOMPARE(queryItem(sfw, "s"), QStringLiteral("comment"));
        QVERIFY(PostMeta::commentsUrl(BooruEngine::Shimmie, kDan, 1).isEmpty());
        QVERIFY(PostMeta::commentsUrl(BooruEngine::Hybooru, kDan, 1).isEmpty());
    }

    void parseNotesShapes() {
        // Danbooru array shape.
        const auto dan = PostMeta::parseNotes(
            QByteArray(R"([{"x":10,"y":20,"width":30,"height":40,)"
                       R"("body":"translated text","is_active":true}])"));
        QCOMPARE(dan.size(), 1);
        QCOMPARE(dan.at(0).x, 10);
        QCOMPARE(dan.at(0).y, 20);
        QCOMPARE(dan.at(0).w, 30);
        QCOMPARE(dan.at(0).h, 40);
        QCOMPARE(dan.at(0).body, QStringLiteral("translated text"));
        // Gelbooru {"note": [...]} object shape; empty bodies dropped.
        const auto gel = PostMeta::parseNotes(
            QByteArray(R"({"note":[{"x":1,"y":2,"width":3,"height":4,)"
                       R"("body":"hi"},{"x":0,"y":0,"width":0,"height":0,)"
                       R"("body":""}]})"));
        QCOMPARE(gel.size(), 1);
        QCOMPARE(gel.at(0).body, QStringLiteral("hi"));
        QVERIFY(PostMeta::parseNotes(QByteArray("nope")).isEmpty());
        QVERIFY(PostMeta::parseNotes(QByteArray("{}")).isEmpty());
    }
};

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    TestPostMeta tc;
    return QTest::qExec(&tc, argc, argv);
}
#include "test_postmeta.moc"
