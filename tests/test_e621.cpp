// e621 engine: Danbooru-fork JSON shapes, verified live 2026-11-21
// (anonymous). Search is a bare array, detail a single object; tags
// arrive flat (no category splits); files carry jpg+webp renditions.
#include "booruengine.h"
#include "tagsuggest.h"

#include <QCoreApplication>
#include <QUrlQuery>
#include <QtTest>

namespace {
QByteArray searchFixture() {
    return QByteArray(
        R"([{"id":6872536,"created_at":"2026-11-21T02:40:21.949-05:00",)"
        R"("files":{"meta":{"md5":"29aB63cA29cB46eCaD9cF2eFdFe4B2a64","ext":"jpg","size":2448000},)"
        R"("original":{"width":6000,"height":7000,"url":"https://static1.e621.net/data/29/ab/29ab63ca29cb46ecad9cf2efdfe4b2a64.jpg"},)"
        R"("preview":{"width":256,"height":299,"jpg":"https://static1.e621.net/data/preview/29/ab/29ab63ca29cb46ecad9cf2efdfe4b2a64.jpg","webp":"https://static1.e621.net/data/preview/29/ab/29ab63ca29cb46ecad9cf2efdfe4b2a64.webp"},)"
        R"("sample":{"width":850,"height":992,"jpg":"https://static1.e621.net/data/sample/29/ab/29ab63ca29cb46ecad9cf2efdfe4b2a64.jpg","webp":"https://static1.e621.net/data/sample/29/ab/29ab63ca29cb46ecad9cf2efdfe4b2a64.webp"}},)"
        R"("stats":{"score":{"up":5,"down":1,"total":4},"fav_count":2,"comment_count":3},"flags":{"deleted":false},)"
        R"("has":{"notes":true},"relationships":{"parent_id":42,"children":[]},"pools":[],"rating":"e","sources":["https://www.pixiv.net/artworks/1"],)"
        R"("description":"dtext *body*",)"
        R"("tags":["animated","anthro","canid","canine","female","male","video"]},)"
        R"({"id":2,"created_at":"2026-11-21T02:40:22.000-05:00",)"
        R"("files":{"meta":{"md5":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb","ext":"webm","size":10},)"
        R"("original":{"width":640,"height":480,"url":"https://static1.e621.net/data/bb/bb/bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb.webm"},)"
        R"("preview":{"width":256,"height":192,"jpg":"https://static1.e621.net/data/preview/bb/bb/bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb.jpg"},)"
        R"("sample":{"width":640,"height":480,"jpg":"https://static1.e621.net/data/sample/bb/bb/bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb.jpg"}},)"
        R"("stats":{"score":{"up":0,"down":0,"total":0},"fav_count":0,"comment_count":0},"flags":{"deleted":false},)"
        R"("has":{"notes":false},"relationships":{"parent_id":null,"children":[]},"pools":[],"rating":"q","sources":[],)"
        R"("description":"","tags":["solo"]},)"
        R"({"id":3,"rating":"s","tags":[],"files":{"meta":{"md5":"cccccccccccccccccccccccccccccccc","ext":"jpg"},"original":{"width":1,"height":1,"url":""}}})"
        R"(])");
}
const QString kBase = QStringLiteral("https://e621.net");
} // namespace

class TestE621 : public QObject {
    Q_OBJECT
private slots:
    void searchParsing() {
        const BooruEngineOps *ops = engineOps(BooruEngine::E621);
        const SearchResult r = ops->parseSearch(
            QStringLiteral("e621"), {BooruEngine::E621, kBase, {}},
            searchFixture());
        QVERIFY(r.ok());
        // The file-less row is skipped.
        QCOMPARE(r.posts.size(), 2);
        const Post still = r.posts.at(0);
        QCOMPARE(still.id, 6872536LL);
        QCOMPARE(still.ext, QStringLiteral("jpg"));
        QCOMPARE(still.fileUrl,
                 QStringLiteral("https://static1.e621.net/data/29/ab/"
                                "29ab63ca29cb46ecad9cf2efdfe4b2a64.jpg"));
        QCOMPARE(still.sampleUrl,
                 QStringLiteral("https://static1.e621.net/data/sample/29/ab/"
                                "29ab63ca29cb46ecad9cf2efdfe4b2a64.jpg"));
        QCOMPARE(still.previewUrl,
                 QStringLiteral("https://static1.e621.net/data/preview/29/ab/"
                                "29ab63ca29cb46ecad9cf2efdfe4b2a64.jpg"));
        QCOMPARE(still.postUrl, kBase + QStringLiteral("/posts/6872536"));
        QCOMPARE(still.width, 6000);
        QCOMPARE(still.height, 7000);
        QCOMPARE(still.rating, QStringLiteral("explicit"));
        QCOMPARE(still.score, 4); // stats.score.total
        QCOMPARE(still.commentCount, 3);
        QVERIFY(still.hasComments);
        QVERIFY(still.hasNotes);
        QCOMPARE(still.source,
                 QStringLiteral("https://www.pixiv.net/artworks/1"));
        QCOMPARE(still.date,
                 QStringLiteral("2026-11-21T02:40:21.949-05:00"));
        QCOMPARE(still.siteKey, QStringLiteral("e621"));
        QVERIFY(still.isRemote && still.hasMeta);
        // Flat tags: everything general + untyped for the classifier.
        QVERIFY(still.tagsGeneral.split(u' ').contains(QStringLiteral("canine")));
        QCOMPARE(still.untyped, still.tagsGeneral);
        QVERIFY(!still.tagsCharacter.split(u' ').contains(QStringLiteral("canine")));
        // md5 normalized (mixed case in the fixture).
        QCOMPARE(still.rawJson.value(QStringLiteral("md5")).toString(),
                 QStringLiteral("29ab63ca29cb46ecad9cf2efdfe4b2a64"));
        // Variants affordance for the viewer (rawJson parent/children).
        QCOMPARE(still.rawJson.value(QStringLiteral("parent_id")).toVariant().toLongLong(), 42LL);
        // Questionable rating maps; video ext routes on meta ext.
        QCOMPARE(r.posts.at(1).rating, QStringLiteral("questionable"));
        QCOMPARE(r.posts.at(1).ext, QStringLiteral("webm"));
    }

    void urls() {
        const BooruEngineOps *ops = engineOps(BooruEngine::E621);
        const BooruEngineOps::Context ctx{BooruEngine::E621, kBase, {}};
        const QUrlQuery second(QUrl(ops->searchUrl(
            ctx, {QStringLiteral("canine")}, 2, 40)).query());
        QCOMPARE(second.queryItemValue(QStringLiteral("tags")),
                 QStringLiteral("canine"));
        QCOMPARE(second.queryItemValue(QStringLiteral("page")), QStringLiteral("2"));
        QCOMPARE(second.queryItemValue(QStringLiteral("limit")), QStringLiteral("40"));
        const QUrlQuery bare(QUrl(ops->searchUrl(ctx, {}, 1, 40)).query());
        QVERIFY(!bare.hasQueryItem(QStringLiteral("tags")));
        QCOMPARE(ops->refreshUrl(ctx, 6872536).toString(),
                 kBase + QStringLiteral("/posts/6872536.json"));
        // Credentials ride as login/api_key query params.
        const BooruEngineOps::Context authed{BooruEngine::E621, kBase,
                                            {QStringLiteral("user"),
                                             QStringLiteral("secret")}};
        const QUrlQuery auth(QUrl(ops->searchUrl(authed, {}, 1, 40)).query());
        QCOMPARE(auth.queryItemValue(QStringLiteral("login")), QStringLiteral("user"));
        QCOMPARE(auth.queryItemValue(QStringLiteral("api_key")), QStringLiteral("secret"));
        QCOMPARE(RemotePosts::engineKey(BooruEngine::E621),
                 QStringLiteral("e621"));
        QCOMPARE(RemotePosts::engineForKey(QStringLiteral("e926")),
                 BooruEngine::E621); // SFW mirror shares the engine
        QCOMPARE(RemotePosts::engineLabel(BooruEngine::E621),
                 QStringLiteral("e621"));
        QVERIFY(RemotePosts::needsTagHydration(BooruEngine::Hybooru));
        QVERIFY(!RemotePosts::needsTagHydration(BooruEngine::E621));
    }

    void translateKeepsNativeSort() {
        // Rating spellings pass through (s/q/e + safe); sort: has no
        // native spelling and drops (client-side sort still applies).
        QCOMPARE(RemotePosts::translateSearchTags(
                     {QStringLiteral("canine"),
                      QStringLiteral("rating:explicit"),
                      QStringLiteral("order:score")},
                     BooruEngine::E621),
                 QStringList({QStringLiteral("canine"),
                              QStringLiteral("rating:explicit"),
                              QStringLiteral("order:score")}));
        QCOMPARE(RemotePosts::translateSearchTags(
                     {QStringLiteral("canine"),
                      QStringLiteral("sort:score")},
                     BooruEngine::E621),
                 QStringList({QStringLiteral("canine")}));
    }

    void sortMapsServerSide() {
        QCOMPARE(RemotePosts::sortSearchTag(QStringLiteral("score-desc"),
                                            BooruEngine::E621),
                 QStringLiteral("order:score"));
        QCOMPARE(RemotePosts::sortSearchTag(QStringLiteral("id-desc"),
                                            BooruEngine::E621),
                 QStringLiteral(""));
        QCOMPARE(RemotePosts::sortSearchTag(QStringLiteral("random"),
                                            BooruEngine::E621),
                 QStringLiteral(""));
    }

    void tagsParsing() {
        // Autocomplete: [{name, post_count, category}] with e621
        // numbering; species/lore read as native ("custom").
        const auto out = TagSuggester::parse(
            BooruEngine::E621,
            QByteArray(R"json([{"name":"canine","post_count":1693712,"category":5},)json"
                       R"json({"name":"hi_res","post_count":4351450,"category":7},)json"
                       R"json({"name":"incest_(lore)","post_count":71792,"category":8},)json"
                       R"json({"name":"hatsune_miku","post_count":1,"category":4}])json"));
        QCOMPARE(out.size(), 4);
        QCOMPARE(out.at(0).tag, QStringLiteral("canine"));
        QCOMPARE(out.at(0).bucket, QStringLiteral("custom"));
        QCOMPARE(out.at(1).bucket, QStringLiteral("meta"));
        QCOMPARE(out.at(2).bucket, QStringLiteral("custom"));
        QCOMPARE(out.at(3).bucket, QStringLiteral("character"));
    }
};

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    TestE621 tc;
    return QTest::qExec(&tc, argc, argv);
}
#include "test_e621.moc"
