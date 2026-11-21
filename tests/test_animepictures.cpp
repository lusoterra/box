// AnimePictures engine: public v3 JSON API (+ tagless summaries that
// hydrate through detail refresh). Shapes verified live 2026-11-21;
// tag-type mapping follows Boorusama's client (1 character, 4 author,
// 3/5/6 copyright, 2/7/0 general).
#include "booruengine.h"
#include "tagsuggest.h"

#include <QCoreApplication>
#include <QUrlQuery>
#include <QtTest>

namespace {
QByteArray searchFixture() {
    return QByteArray(
        R"({"posts_per_page":2,"response_posts_count":2,"page_number":0,)"
        R"("posts":[{"id":935680,"md5":"1909f43ec08e3e76c610175ca07f8e0e","width":4096,)"
        R"("height":1855,"pubtime":"2026-11-21T01:57:22.643938","score":0.0,)"
        R"("score_number":2,"size":865904,"erotics":1,"ext":".jpg"},)"
        R"({"id":2,"md5":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","width":640,"height":480,)"
        R"("pubtime":"2026-11-21T01:00:00.000000","score":1.5,"score_number":7,)"
        R"("size":10,"erotics":0,"ext":".png"}],)"
        R"("posts_count":671257,"max_pages":335628})");
}
QByteArray detailFixture() {
    return QByteArray(
        R"({"post":{"id":935680,"md5":"1909f43ec08e3e76c610175ca07f8e0e","width":4096,)"
        R"("height":1855,"pubtime":"2026-11-21T01:57:22.643938","score":0.0,"score_number":2,)"
        R"("size":865904,"erotics":1,"ext":".jpg",)"
        R"("small_preview":"https://opreviews.anime-pictures.net/190/1909f43ec08e3e76c610175ca07f8e0e_sp.avif",)"
        R"("medium_preview":"https://opreviews.anime-pictures.net/190/1909f43ec08e3e76c610175ca07f8e0e_cp.avif",)"
        R"("big_preview":"https://opreviews.anime-pictures.net/190/1909f43ec08e3e76c610175ca07f8e0e_bp.avif"},)"
        R"("source":{"kind":"external","url":"https://x.com/u/status/1"},)"
        R"("tags":[{"tag":{"id":1,"tag":"hoshimachi suisei","num":486,"type":1}},)"
        R"({"tag":{"id":2,"tag":"tang-du","num":34,"type":4}},)"
        R"({"tag":{"id":3,"tag":"hololive","num":12614,"type":6}},)"
        R"({"tag":{"id":4,"tag":"long hair","num":528584,"type":2}},)"
        R"({"tag":{"id":5,"tag":"girl","num":750542,"type":7}}]})");
}
const QString kBase = QStringLiteral("https://anime-pictures.net");
} // namespace

class TestAnimePictures : public QObject {
    Q_OBJECT
private slots:
    void searchParsing() {
        const BooruEngineOps *ops = engineOps(BooruEngine::AnimePictures);
        const SearchResult r = ops->parseSearch(
            QStringLiteral("animepictures"),
            {BooruEngine::AnimePictures, kBase, {}}, searchFixture());
        QVERIFY(r.ok());
        QCOMPARE(r.posts.size(), 2);
        // Tagless summaries hydrate later; media URLs reconstruct now.
        const Post still = r.posts.at(0);
        QCOMPARE(still.id, 935680LL);
        QCOMPARE(still.ext, QStringLiteral("jpg")); // dot stripped
        QVERIFY(still.tags.trimmed().isEmpty());
        QVERIFY(still.untyped.isEmpty());
        QCOMPARE(still.fileUrl,
                 QStringLiteral("https://oimages.anime-pictures.net/190/"
                                "1909f43ec08e3e76c610175ca07f8e0e.jpg"));
        QCOMPARE(still.previewUrl,
                 QStringLiteral("https://opreviews.anime-pictures.net/190/"
                                "1909f43ec08e3e76c610175ca07f8e0e_sp.avif"));
        QCOMPARE(still.sampleUrl,
                 QStringLiteral("https://opreviews.anime-pictures.net/190/"
                                "1909f43ec08e3e76c610175ca07f8e0e_bp.avif"));
        QCOMPARE(still.postUrl,
                 kBase + QStringLiteral("/pictures/view_post/935680"));
        QCOMPARE(still.width, 4096);
        QCOMPARE(still.height, 1855);
        QCOMPARE(still.rating, QStringLiteral("sensitive")); // erotics 1
        QCOMPARE(still.score, 2);                            // score_number
        QCOMPARE(still.date,
                 QStringLiteral("2026-11-21T01:57:22.643938"));
        QCOMPARE(still.siteKey, QStringLiteral("animepictures"));
        QVERIFY(still.isRemote && still.hasMeta);
        QCOMPARE(still.rawJson.value(QStringLiteral("md5")).toString(),
                 QStringLiteral("1909f43ec08e3e76c610175ca07f8e0e"));
        // erotics 0 reads general; png ext survives.
        QCOMPARE(r.posts.at(1).rating, QStringLiteral("general"));
        QCOMPARE(r.posts.at(1).ext, QStringLiteral("png"));
    }

    void detailParsing() {
        bool ok = false;
        const BooruEngineOps *ops = engineOps(BooruEngine::AnimePictures);
        const QJsonObject raw =
            ops->parseSingle(detailFixture(), ok);
        QVERIFY(ok);
        const Post p =
            RemotePosts::fromAnimePictures(raw, kBase);
        QCOMPARE(p.id, 935680LL);
        QCOMPARE(p.source, QStringLiteral("https://x.com/u/status/1"));
        // Numeric types onto the classic buckets; spaced names underscore.
        QVERIFY(p.tagsCharacter.split(u' ').contains(
            QStringLiteral("hoshimachi_suisei")));
        QVERIFY(p.tagsArtist.split(u' ').contains(QStringLiteral("tang-du")));
        QCOMPARE(p.tagsArtist.split(u' ').size(), 1);
        QVERIFY(p.tagsCopyright.split(u' ').contains(QStringLiteral("hololive")));
        QVERIFY(p.tagsGeneral.split(u' ').contains(QStringLiteral("long_hair")));
        QVERIFY(p.tagsGeneral.split(u' ').contains(QStringLiteral("girl")));
        QVERIFY(!p.tagsCharacter.contains(u' ')); // no raw spaces
        QVERIFY(p.untyped.isEmpty()); // fully bucketed: nothing refetches
        QVERIFY(!p.tags.trimmed().isEmpty());
    }

    void urls() {
        const BooruEngineOps *ops = engineOps(BooruEngine::AnimePictures);
        const BooruEngineOps::Context ctx{BooruEngine::AnimePictures, kBase,
                                          {}};
        // Pages are 0-based server-side; underscores go back to spaces.
        const QUrlQuery second(QUrl(ops->searchUrl(
            ctx, {QStringLiteral("long_hair"), QStringLiteral("-hat")}, 2,
            40)).query());
        QCOMPARE(second.queryItemValue(QStringLiteral("search_tag")),
                 QStringLiteral("long hair"));
        QVERIFY(!second.queryItemValue(QStringLiteral("search_tag"))
                     .split(u' ')
                     .contains(QStringLiteral("hat")));
        QVERIFY(second.queryItemValue(QStringLiteral("denied_tags"))
                    .split(u' ')
                    .contains(QStringLiteral("hat")));
        QCOMPARE(second.queryItemValue(QStringLiteral("page")), QStringLiteral("1"));
        QCOMPARE(second.queryItemValue(QStringLiteral("posts_per_page")),
                 QStringLiteral("40"));
        QCOMPARE(second.queryItemValue(QStringLiteral("lang")), QStringLiteral("en"));
        // API subdomain, not the site root.
        QVERIFY(QUrl(ops->searchUrl(ctx, {}, 1, 40)).host().startsWith(
            QStringLiteral("api.")));
        QCOMPARE(ops->refreshUrl(ctx, 935680).toString(),
                 QStringLiteral("https://api.anime-pictures.net/api/v3/posts/935680"));
        QCOMPARE(RemotePosts::engineKey(BooruEngine::AnimePictures),
                 QStringLiteral("animepictures"));
        QCOMPARE(RemotePosts::engineForKey(QStringLiteral("animepictures")),
                 BooruEngine::AnimePictures);
        QCOMPARE(RemotePosts::engineLabel(BooruEngine::AnimePictures),
                 QStringLiteral("AnimePictures"));
        QVERIFY(RemotePosts::needsTagHydration(BooruEngine::AnimePictures));
    }

    void translateRewritesMedia() {
        // Verified rewrites (Grabber model.ts); rating/sort/order drop
        // server-side (client net still filters).
        QCOMPARE(RemotePosts::translateSearchTags(
                     {QStringLiteral("sky"),
                      QStringLiteral("filetype:jpg,png"),
                      QStringLiteral("width:>=1280"),
                      QStringLiteral("rating:explicit")},
                     BooruEngine::AnimePictures),
                 QStringList({QStringLiteral("sky"),
                              QStringLiteral("ext_jpg=jpg"),
                              QStringLiteral("ext_png=png"),
                              QStringLiteral("res_x=1280"),
                              QStringLiteral("res_x_n=1")}));
        // Negated filetype drops (client ext net filters instead).
        QCOMPARE(RemotePosts::translateSearchTags(
                     {QStringLiteral("-filetype:mp4")},
                     BooruEngine::AnimePictures),
                 QStringList());
    }

    void sortStaysClientSide() {
        // No verified server-side sort spellings: everything client-side.
        QCOMPARE(RemotePosts::sortSearchTag(QStringLiteral("score-desc"),
                                            BooruEngine::AnimePictures),
                 QStringLiteral(""));
        QCOMPARE(RemotePosts::sortSearchTag(QStringLiteral("id-desc"),
                                            BooruEngine::AnimePictures),
                 QStringLiteral(""));
    }

    void tagsParsing() {
        // Autocomplete: {tags: [{t (bold-wrapped), t2, c, id}]}.
        const auto out = TagSuggester::parse(
            BooruEngine::AnimePictures,
            QByteArray(R"({"tags":[{"t":"<b>long</b> hair","t2":"","c":2,"id":54},)"
                       R"({"t":"hoshimachi suisei","t2":"","c":1,"id":175391},)"
                       R"({"t":"tang-du","t2":"","c":4,"id":196530}]})"));
        QCOMPARE(out.size(), 3);
        QCOMPARE(out.at(0).tag, QStringLiteral("long_hair"));
        QCOMPARE(out.at(0).bucket, QStringLiteral("general"));
        QCOMPARE(out.at(1).bucket, QStringLiteral("character"));
        QCOMPARE(out.at(2).bucket, QStringLiteral("artist"));
    }
};

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    TestAnimePictures tc;
    return QTest::qExec(&tc, argc, argv);
}
#include "test_animepictures.moc"
