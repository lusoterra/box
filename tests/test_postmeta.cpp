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
        // Gelbooru dapi notes answer 401 with an empty body anonymously
        // (live 2026-11-21), so notes always come from the post page -
        // never from dapi, regardless of any has_notes hint.
        QVERIFY(gel.toString().contains(QStringLiteral("page=post")));
        QCOMPARE(queryItem(gel, "s"), QStringLiteral("view"));
        QCOMPARE(queryItem(gel, "id"), QStringLiteral("42"));
        // Safebooru rides the same page shape (not Moebooru's).
        const QUrl sfw =
            PostMeta::notesUrl(BooruEngine::Safebooru, kSfw, 42, {}, {});
        QVERIFY(sfw.toString().contains(QStringLiteral("page=post")));
        QVERIFY(sfw.toString().startsWith(kSfw));
        QCOMPARE(PostMeta::notesUrl(BooruEngine::Moebooru, kMoe, 42).toString(),
                 kMoe + QStringLiteral("/note.json?post_id=42"));
        // HyBooru annotations ride the single-post detail object.
        QCOMPARE(PostMeta::notesUrl(BooruEngine::Hybooru, kDan, 42).toString(),
                 kDan + QStringLiteral("/api/post/42"));
        // e621 is a Danbooru fork: same /notes.json (verified live).
        QCOMPARE(PostMeta::notesUrl(BooruEngine::E621,
                                   QStringLiteral("https://e621.net"), 42)
                     .toString(),
                 QStringLiteral("https://e621.net/notes.json?"
                                "search[post_id]=42&limit=100"));
        // No separate notes endpoint here (AnimePictures must not fall
        // through to Moebooru's URL: a 404 would fail the whole fetch).
        QVERIFY(PostMeta::notesUrl(BooruEngine::Szurubooru, kDan, 1).isEmpty());
        QVERIFY(PostMeta::notesUrl(BooruEngine::Shimmie, kDan, 1).isEmpty());
        QVERIFY(PostMeta::notesUrl(BooruEngine::Philomena, kDan, 1).isEmpty());
        QVERIFY(PostMeta::notesUrl(BooruEngine::BooruOnRails, kDan, 1).isEmpty());
        QVERIFY(PostMeta::notesUrl(BooruEngine::AnimePictures, kDan, 1).isEmpty());
    }

    void commentsUrls() {
        QVERIFY(PostMeta::commentsUrl(BooruEngine::Danbooru, kDan, 7)
                    .toString()
                    .contains(QStringLiteral("/comments.json")));
        const QUrl sfw =
            PostMeta::commentsUrl(BooruEngine::Safebooru, kSfw, 7, {}, {});
        QCOMPARE(queryItem(sfw, "s"), QStringLiteral("comment"));
        // Szurubooru scopes through the query language: ?postId= is
        // ignored and returns the whole feed.
        // (Live jej.lol 2026-11-09: ?postId=35460 -> total 265 across
        // posts; ?query=post:35460 -> total 3, all for 35460.)
        const QUrl szuru = PostMeta::commentsUrl(
            BooruEngine::Szurubooru, QStringLiteral("https://jej.lol"), 7);
        QVERIFY(szuru.toString().startsWith(
            QStringLiteral("https://jej.lol/api/comments/")));
        QCOMPARE(queryItem(szuru, "query"), QStringLiteral("post:7"));
        QVERIFY(queryItem(szuru, "postId").isEmpty());
        QVERIFY(PostMeta::commentsUrl(BooruEngine::Shimmie, kDan, 1).isEmpty());
        QVERIFY(PostMeta::commentsUrl(BooruEngine::Hybooru, kDan, 1).isEmpty());
        QVERIFY(PostMeta::commentsUrl(BooruEngine::Philomena, kDan, 1).isEmpty());
        QVERIFY(PostMeta::commentsUrl(BooruEngine::BooruOnRails, kDan, 1).isEmpty());
        // e621 shares Danbooru's /comments.json (verified live);
        // AnimePictures has no comment API (no Moebooru fallthrough).
        QVERIFY(PostMeta::commentsUrl(BooruEngine::E621,
                                     QStringLiteral("https://e621.net"), 7)
                    .toString()
                    .contains(QStringLiteral("/comments.json")));
        QVERIFY(PostMeta::commentsUrl(BooruEngine::AnimePictures, kDan, 1).isEmpty());
    }

    void commentaryUrlPerEngine() {
        const QUrl dan = PostMeta::commentaryUrl(
            BooruEngine::Danbooru,
            QStringLiteral("https://danbooru.donmai.us"), 42);
        QVERIFY(!dan.isEmpty());
        QCOMPARE(QUrlQuery(dan).queryItemValue(
                     QStringLiteral("search[post_id]")),
                 QStringLiteral("42"));
        QCOMPARE(PostMeta::commentaryUrl(
                     BooruEngine::Philomena,
                     QStringLiteral("https://derpibooru.org"), 42)
                     .toString(),
                 QStringLiteral("https://derpibooru.org/api/v1/json/images/42"));
        QCOMPARE(PostMeta::commentaryUrl(
                     BooruEngine::BooruOnRails,
                     QStringLiteral("https://twibooru.org"), 42)
                     .toString(),
                 QStringLiteral("https://twibooru.org/api/v3/posts/42"));
        QCOMPARE(PostMeta::commentaryUrl(
                     BooruEngine::Hybooru,
                     QStringLiteral("https://booru.example"), 42)
                     .toString(),
                 QStringLiteral("https://booru.example/api/post/42"));
        for (BooruEngine e : {BooruEngine::Gelbooru, BooruEngine::Safebooru,
                              BooruEngine::Moebooru, BooruEngine::Szurubooru,
                              BooruEngine::Shimmie, BooruEngine::Nuuru})
            QVERIFY(PostMeta::commentaryUrl(
                        e, QStringLiteral("https://gelbooru.com"), 42)
                        .isEmpty());
    }

    void parseCommentaryEngines() {
        bool ok = false;
        // Philomena v1 image envelope.
        const auto derpi = PostMeta::parseCommentary(
            BooruEngine::Philomena,
            QByteArray(R"({"image":{"description":"**hello**"}})"), &ok);
        QVERIFY(ok);
        QCOMPARE(derpi.size(), 1);
        QCOMPARE(derpi.at(0).body, QStringLiteral("**hello**"));
        // Philomena v3 post envelope.
        const auto twi = PostMeta::parseCommentary(
            BooruEngine::BooruOnRails,
            QByteArray(R"({"post":{"description":"twibody"}})"), &ok);
        QVERIFY(ok);
        QCOMPARE(twi.size(), 1);
        QCOMPARE(twi.at(0).body, QStringLiteral("twibody"));
        // HyBooru: rect != null is an annotation; translated commentary
        // wins over the original when both are present.
        const auto hyb = PostMeta::parseCommentary(
            BooruEngine::Hybooru,
            QByteArray(R"({"notes":[)"
                       R"({"label":"danbooru artist commentary",)"
                       R"("note":"original","rect":null},)"
                       R"({"label":"danbooru translated artist commentary",)"
                       R"("note":"translated","rect":null},)"
                       R"({"label":null,"note":"annotation",)"
                       R"("rect":{"left":1,"top":2,"width":3,"height":4}}]})"),
            &ok);
        QVERIFY(ok);
        QCOMPARE(hyb.size(), 1);
        QCOMPARE(hyb.at(0).body, QStringLiteral("translated"));
        // No commentary labels: every non-annotation note survives.
        const auto generic = PostMeta::parseCommentary(
            BooruEngine::Hybooru,
            QByteArray(R"({"notes":[{)"
                       R"("label":"personal","note":"one","rect":null},)"
                       R"({"label":"extra","note":"two","rect":null}]})"),
            &ok);
        QVERIFY(ok);
        QCOMPARE(generic.size(), 1);
        QCOMPARE(generic.at(0).body, QStringLiteral("one\n\ntwo"));
        // Empty/blank descriptions stay hidden.
        QVERIFY(PostMeta::parseCommentary(BooruEngine::Philomena,
                                          QByteArray(R"({"image":{"description":""}})"),
                                          &ok)
                    .isEmpty());
        QVERIFY(ok);
        QVERIFY(PostMeta::parseCommentary(BooruEngine::Hybooru,
                                          QByteArray("nope"), &ok)
                    .isEmpty());
        QVERIFY(!ok);
    }

    void parseCommentaryShapes() {
        const auto c = PostMeta::parseCommentary(
            QByteArray(R"({"artist_commentary":[{)"
                       R"("original_title":"JP title",)"
                       R"("original_description":"JP body",)"
                       R"("translated_title":"EN title",)"
                       R"("translated_description":"EN body"}]})"));
        QCOMPARE(c.size(), 1);
        QVERIFY(c.at(0).body.contains(QStringLiteral("EN title")));
        QVERIFY(c.at(0).body.contains(QStringLiteral("EN body")));
        QVERIFY(!c.at(0).body.contains(QStringLiteral("JP")));
        bool ok = false;
        QVERIFY(PostMeta::parseCommentary(QByteArray("nope"), &ok).isEmpty());
        QVERIFY(!ok);
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
        // Answered-ness: valid JSON (even empty) counts as answered;
        // challenge HTML does not (callers retry instead of caching).
        bool ok = false;
        QVERIFY(PostMeta::parseNotes(QByteArray("[]"), &ok).isEmpty());
        QVERIFY(ok);
        QVERIFY(PostMeta::parseNotes(QByteArray("<html>wall</html>"), &ok).isEmpty());
        QVERIFY(!ok);
    }

    void parseNotesScopedToPost() {
        // Endpoints that ignore the post filter dump the whole site's
        // annotations: rows stamped with another post_id must never show
        // up on this post (it looked like "notes on almost every post").
        const QByteArray dump =
            R"({"note":[{"post_id":11,"body":"not mine",)"
            R"("x":1,"y":2,"width":3,"height":4},)"
            R"({"post_id":42,"body":"mine","x":5,"y":6,"width":7,"height":8},)"
            R"({"body":"unstamped","x":9,"y":10,"width":11,"height":12}]})";
        bool ok = false;
        const auto scoped = PostMeta::parseNotes(dump, &ok, 42);
        QVERIFY(ok);
        QCOMPARE(scoped.size(), 2); // this post + the unstamped row
        QCOMPARE(scoped.at(0).body, QStringLiteral("mine"));
        QCOMPARE(scoped.at(1).body, QStringLiteral("unstamped"));
        // No scope asked: nothing filtered (back-compat).
        QCOMPARE(PostMeta::parseNotes(dump, &ok).size(), 3);
        // Deleted annotations (is_active=false) never render.
        const auto inactive = PostMeta::parseNotes(
            QByteArray(R"([{"post_id":42,"body":"gone","is_active":false},)"
                       R"({"post_id":42,"body":"shown","is_active":true}])"),
            &ok, 42);
        QCOMPARE(inactive.size(), 1);
        QCOMPARE(inactive.at(0).body, QStringLiteral("shown"));
    }

    void parseNotesXml() {
        // Gelbooru dapi ignores json=1 for the note table on some builds
        // (live safebooru.org) and answers XML: parse that too instead of
        // failing every note fetch on the site.
        const QByteArray xml =
            R"xml(<?xml version="1.0" encoding="UTF-8"?><notes type="array">)xml"
            R"xml(<note post_id="7" x="92" y="540" width="75" height="104" )xml"
            R"xml(body="translation one" is_active="true" />)xml"
            R"xml(<note post_id="9" x="1" y="2" width="3" height="4" )xml"
            R"xml(body="other post" is_active="true" />)</notes>)xml";
        bool ok = false;
        const auto notes = PostMeta::parseNotes(xml, &ok, 7);
        QVERIFY(ok);
        QCOMPARE(notes.size(), 1);
        QCOMPARE(notes.at(0).x, 92);
        QCOMPARE(notes.at(0).y, 540);
        QCOMPARE(notes.at(0).w, 75);
        QCOMPARE(notes.at(0).h, 104);
        QCOMPARE(notes.at(0).body, QStringLiteral("translation one"));
        // Empty table = answered (no retry loop on quiet posts).
        QVERIFY(PostMeta::parseNotes(
                    QByteArray(R"xml(<notes type="array"/>)xml"), &ok, 7)
                    .isEmpty());
        QVERIFY(ok);
    }
    void parseNotesHybooru() {
        // Hydrus notes: rect values are percentages of the original
        // image (0..100), converted to pixels with the detail dimensions.
        const QByteArray json =
            QByteArrayLiteral("{\"id\":19099,\"width\":1000,\"height\":500,"
                              "\"notes\":["
                              "{\"label\":null,\"note\":\"boxed text\","
                              "\"rect\":{\"left\":10,\"top\":20,"
                              "\"width\":30,\"height\":40}},"
                              "{\"label\":\"danbooru translated artist "
                              "commentary\",\"note\":\"commentary text\","
                              "\"rect\":null}]}");
        bool ok = false;
        const auto notes = PostMeta::parseNotes(json, &ok, 19099);
        QVERIFY(ok);
        QCOMPARE(notes.size(), 1); // rect=null is commentary, not an overlay
        QCOMPARE(notes.at(0).x, 100);
        QCOMPARE(notes.at(0).y, 100);
        QCOMPARE(notes.at(0).w, 300);
        QCOMPARE(notes.at(0).h, 200);
        QCOMPARE(notes.at(0).body, QStringLiteral("boxed text"));
    }

    void parseNotesGelbooruHtml() {
        // gelbooru.com embeds <article data-*> annotations in the post page.
        const QByteArray gel =
            QByteArrayLiteral("<html><section id=\"notes\">"
                              "<article data-width=\"129\" data-height=\"261\" "
                              "data-x=\"2073\" data-y=\"221\" "
                              "data-id=\"739884\" "
                              "data-body=\"Did you moan Saori?\">"
                              "Did you moan Saori?</article>"
                              "<article data-width=\"283\" data-height=\"370\" "
                              "data-x=\"1609\" data-y=\"385\" "
                              "data-body=\"Ah!&#039;q&quot;\">x</article>"
                              "</section></html>");
        bool ok = false;
        auto notes = PostMeta::parseNotes(gel, &ok, 1);
        QVERIFY(ok);
        QCOMPARE(notes.size(), 2);
        QCOMPARE(notes.at(0).x, 2073);
        QCOMPARE(notes.at(0).y, 221);
        QCOMPARE(notes.at(0).w, 129);
        QCOMPARE(notes.at(0).h, 261);
        QCOMPARE(notes.at(0).body, QStringLiteral("Did you moan Saori?"));
        QCOMPARE(notes.at(1).body, QStringLiteral("Ah!'q\""));
        // An empty notes section is a real answer, not a failed fetch.
        bool emptyOk = false;
        QVERIFY(PostMeta::parseNotes(
                    QByteArrayLiteral(
                        "<html><section id=\"notes\"></section></html>"),
                    &emptyOk, 1)
                    .isEmpty());
        QVERIFY(emptyOk);
        // safebooru.org renders note-box/note-body divs instead.
        const QByteArray sfw = QByteArrayLiteral(
            "<div id=\"note-container\">"
            "<div class=\"note-box\" style=\"width: 57px; height: 749px; "
            "top: 290px; left: 732px; display: block;\" "
            "id=\"note-box-140228\"></div>"
            "<div style=\"top: 290px; left: 732px;\" class=\"note-body\" "
            "id=\"note-body-140228\" title=\"Click to edit\">"
            "Her both arms &amp; more</div></div>");
        notes = PostMeta::parseNotes(sfw, &ok, 1);
        QVERIFY(ok);
        QCOMPARE(notes.size(), 1);
        QCOMPARE(notes.at(0).x, 732);
        QCOMPARE(notes.at(0).y, 290);
        QCOMPARE(notes.at(0).w, 57);
        QCOMPARE(notes.at(0).h, 749);
        QCOMPARE(notes.at(0).body, QStringLiteral("Her both arms & more"));
    }

    void parseNotesGelbooruLiveShape() {
        // Live gelbooru.com post 15305371 (2026-11): section carries the
        // notes, bodies are entity-encoded, one spans lines. Guards the
        // "notes don't show" class against markup drift.
        const QByteArray live = QByteArrayLiteral(
            "<section class=\"image-container note-container\">"
            "<div id=\"note-preview\"></div></section>"
            "<section id=\"notes\" style=\"display: none;\">"
            "<article data-width=\"137\" data-height=\"320\" data-x=\"3090\" "
            "data-y=\"1921\" data-id=\"925073\" "
            "data-body=\"I&#039;m cumming...&hearts;\">x</article>"
            "<article data-width=\"424\" data-height=\"965\" data-x=\"2786\" "
            "data-y=\"2939\" data-id=\"925080\" "
            "data-body=\"Hiratsuka-sensei...!?\nYou-... what are you doing...?\">"
            "y</article></section>");
        bool ok = false;
        const auto notes = PostMeta::parseNotes(live, &ok, 15305371);
        QVERIFY(ok);
        QCOMPARE(notes.size(), 2);
        QCOMPARE(notes.at(0).x, 3090);
        QCOMPARE(notes.at(0).y, 1921);
        QCOMPARE(notes.at(0).w, 137);
        QCOMPARE(notes.at(0).h, 320);
        QCOMPARE(notes.at(0).body,
                 QStringLiteral("I'm cumming...\u2665"));
        QVERIFY(notes.at(1).body.contains(QStringLiteral("Hiratsuka-sensei")));
    }

    void parseCommentsShapes() {
        // Danbooru array shape.
        const auto dan = PostMeta::parseComments(
            QByteArray(R"([{"creator_name":"alice","body":"nice shot",)"
                       R"("created_at":"2026-01-01T00:00:00Z",)"
                       R"("creator_name":"alice"},{"body":""}])"));
        QCOMPARE(dan.size(), 1);
        QCOMPARE(dan.at(0).author, QStringLiteral("alice"));
        QCOMPARE(dan.at(0).body, QStringLiteral("nice shot"));
        QCOMPARE(dan.at(0).date, QStringLiteral("2026-01-01T00:00:00Z"));
        // Gelbooru {"comment": [...]} object shape (body under "comment").
        const auto gel = PostMeta::parseComments(
            QByteArray(R"({"comment":[{"author":"bob","comment":"lol"}]})"));
        QCOMPARE(gel.size(), 1);
        QCOMPARE(gel.at(0).author, QStringLiteral("bob"));
        QCOMPARE(gel.at(0).body, QStringLiteral("lol"));
        // Szurubooru {"results": [...]} with nested user + text + date.
        const auto szu = PostMeta::parseComments(
            QByteArray(R"({"results":[{"user":{"name":"carol"},)"
                       R"("text":"pretty","creationTime":"2026-02-02T00:00:00Z"}]})"));
        QCOMPARE(szu.size(), 1);
        QCOMPARE(szu.at(0).author, QStringLiteral("carol"));
        QCOMPARE(szu.at(0).body, QStringLiteral("pretty"));
        QCOMPARE(szu.at(0).date, QStringLiteral("2026-02-02"));
        // Nuuru {"comments": [...]} plural envelope (matches its
        // {"posts": [...]} search shape). Field names are the flexible
        // ones parseComments already accepts.
        const auto nuu = PostMeta::parseComments(
            QByteArray(R"({"comments":[{"author":"dave","body":"cool",)"
                       R"("created_at":"2026-03-03T00:00:00Z"}],)"
                       R"("totalCount":1,"totalPages":1})"));
        QCOMPARE(nuu.size(), 1);
        QCOMPARE(nuu.at(0).author, QStringLiteral("dave"));
        QCOMPARE(nuu.at(0).body, QStringLiteral("cool"));
        // Real Nuuru/soybooru shape: {"items": [...]} with contentRaw and
        // a nested author.userName. This is the live shape (2026-10-31).
        const auto nuuLive = PostMeta::parseComments(
            QByteArray(R"({"items":[{"id":1,"contentRaw":"Keep Leaking",)"
                       R"("createdAt":"2026-10-31T09:56:10.547341Z",)"
                       R"("author":{"userName":"Penisoider"}}],)"
                       R"("totalCount":1})"));
        QCOMPARE(nuuLive.size(), 1);
        QCOMPARE(nuuLive.at(0).author, QStringLiteral("Penisoider"));
        QCOMPARE(nuuLive.at(0).body, QStringLiteral("Keep Leaking"));
        QCOMPARE(nuuLive.at(0).date,
                 QStringLiteral("2026-10-31T09:56:10.547341Z"));
        QVERIFY(PostMeta::parseComments(QByteArray("nope")).isEmpty());
        bool ok = false;
        QVERIFY(PostMeta::parseComments(QByteArray("[]"), &ok).isEmpty());
        QVERIFY(ok);
        QVERIFY(PostMeta::parseComments(QByteArray("<html>wall</html>"), &ok).isEmpty());
        QVERIFY(!ok);
        // Gelbooru dapi XML comment table (same builds as notes), scoped to
        // the requested post.
        const QByteArray xml =
            R"(<?xml version="1.0" encoding="UTF-8"?><comments type="array">)"
            R"(<comment id="1" post_id="7" creator="alice" )"
            R"(created_at="2026-01-01" body="first" />)"
            R"(<comment id="2" post_id="9" creator="bob" )"
            R"(created_at="2026-01-02" body="elsewhere" />)</comments>)";
        const auto xmlc = PostMeta::parseComments(xml, &ok, 7);
        QVERIFY(ok);
        QCOMPARE(xmlc.size(), 1);
        QCOMPARE(xmlc.at(0).author, QStringLiteral("alice"));
        QCOMPARE(xmlc.at(0).body, QStringLiteral("first"));
        QCOMPARE(xmlc.at(0).date, QStringLiteral("2026-01-01"));
        // Unscoped comment feeds (endpoints that ignore the post filter)
        // drop other posts' rows.
        const auto dumps = PostMeta::parseComments(
            QByteArray(R"({"comment":[{"post_id":1,"author":"x","comment":"no"},)"
                       R"({"post_id":7,"author":"y","comment":"yes"}]})"),
            &ok, 7);
        QCOMPARE(dumps.size(), 1);
        QCOMPARE(dumps.at(0).body, QStringLiteral("yes"));
    }
};

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    TestPostMeta tc;
    return QTest::qExec(&tc, argc, argv);
}
#include "test_postmeta.moc"
