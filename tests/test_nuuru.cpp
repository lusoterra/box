// Nuuru engine: search/detail/tag shapes reverse-engineered from the
// served HTML/JS + live SSR embeds (soybooru.com). Plain fetches hit the
// PoW wall, so fixtures are captured shapes, not live calls.
#include "booruengine.h"
#include "tagsuggest.h"

#include <QCoreApplication>
#include <QUrlQuery>
#include <QtTest>

namespace {
// Two-post search page (tags embedded, like the SSR searchResults).
QByteArray searchFixture() {
    return QByteArray(
        R"({"posts":[{"id":305199,"storageIdentifier":"2d93737a01d949faaa45e0d41b2f6f5d","imageHash":"77e0a389beec9311775fc6250bc2e352b2702958249fb6396513dc988640028d","md5Hash":"4f0a3d37c18e61c8550ab82b94ef06a2","sha1Hash":"bee852bbc19048df36ec235ae6af7ae50801f7fab","mimeType":"image/png","fileSize":96864,"originalFileName":"captionless.png","source":null,"description":null,"width":750,"height":700,"durationSeconds":null,"uploadedAt":"2026-10-21T01:51:08.000Z","rating":"safe","score":0,"tags":[{"name":"femjak","category":"variant","postCount":7278},{"name":"nas_focus","category":"meta","postCount":22343},{"name":"kazakh_soy","category":"namefag","postCount":150},{"name":"1soyjak","category":null,"postCount":31395}]},)"
        R"({"id":305200,"storageIdentifier":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","md5Hash":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb","mimeType":"video/mp4","fileSize":9911,"originalFileName":"clip.mp4","width":640,"height":480,"durationSeconds":12,"uploadedAt":"2026-10-21T01:52:00.000Z","rating":"safe","score":3,"tags":[]}],"totalCount":2,"totalPages":1})");
}
const QString kBase = QStringLiteral("https://soybooru.com");
} // namespace

class TestNuuru : public QObject {
    Q_OBJECT
private slots:
    void searchParsing() {
        const BooruEngineOps *ops = engineOps(BooruEngine::Nuuru);
        const SearchResult r = ops->parseSearch(
            QStringLiteral("soy"), {BooruEngine::Nuuru, kBase, {}},
            searchFixture());
        QVERIFY(r.ok());
        QCOMPARE(r.posts.size(), 2);
        const Post still = r.posts.at(0);
        QCOMPARE(still.id, 305199LL);
        QCOMPARE(still.ext, QStringLiteral("png")); // from mimeType
        QCOMPARE(still.fileUrl,
                 kBase + QStringLiteral("/api/booru/posts/305199/file"));
        QCOMPARE(still.previewUrl,
                 kBase + QStringLiteral("/api/booru/posts/305199/thumbnail"));
        QCOMPARE(still.postUrl, kBase + QStringLiteral("/post/view/305199"));
        QCOMPARE(still.width, 750);
        QCOMPARE(still.height, 700);
        QCOMPARE(still.rating, QStringLiteral("general")); // safe
        QCOMPARE(still.siteKey, QStringLiteral("soy"));   // stamped for refresh
        QVERIFY(still.isRemote && still.hasMeta);
        // Native taxonomy preserved...
        QCOMPARE(still.customTags.value(QStringLiteral("variant")),
                 QStringList({QStringLiteral("femjak")}));
        QCOMPARE(still.customTags.value(QStringLiteral("namefag")),
                 QStringList({QStringLiteral("kazakh_soy")}));
        // ...with the obvious folds applied.
        QVERIFY(still.tagsCharacter.split(u' ').contains(QStringLiteral("femjak")));
        QVERIFY(still.tagsMeta.split(u' ').contains(QStringLiteral("nas_focus")));
        QVERIFY(still.tagsGeneral.split(u' ').contains(QStringLiteral("1soyjak")));
        // md5 normalized for download naming.
        QCOMPARE(still.rawJson.value(QStringLiteral("md5")).toString(),
                 QStringLiteral("4f0a3d37c18e61c8550ab82b94ef06a2"));
        // Video mime routes on extension.
        QCOMPARE(r.posts.at(1).ext, QStringLiteral("mp4"));
    }

    void urls() {
        const BooruEngineOps *ops = engineOps(BooruEngine::Nuuru);
        const BooruEngineOps::Context ctx{BooruEngine::Nuuru, kBase, {}};
        // box pages are 1-indexed and so is the API: no offset.
        const QUrlQuery second(QUrl(ops->searchUrl(
            ctx, {QStringLiteral("chudjak")}, 2, 40)).query());
        QCOMPARE(second.queryItemValue(QStringLiteral("page")), QStringLiteral("2"));
        QCOMPARE(second.queryItemValue(QStringLiteral("pageSize")), QStringLiteral("40"));
        QCOMPARE(second.queryItemValue(QStringLiteral("q")),
                 QStringLiteral("chudjak"));
        const QUrlQuery bare(QUrl(ops->searchUrl(ctx, {}, 1, 40)).query());
        QVERIFY(!bare.hasQueryItem(QStringLiteral("q")));
        QCOMPARE(ops->refreshUrl(ctx, 305199).toString(),
                 kBase + QStringLiteral("/api/booru/posts/305199"));
        QCOMPARE(RemotePosts::engineKey(BooruEngine::Nuuru),
                 QStringLiteral("nuuru"));
        QCOMPARE(RemotePosts::engineForKey(QStringLiteral("nuuru")),
                 BooruEngine::Nuuru);
        QCOMPARE(RemotePosts::engineLabel(BooruEngine::Nuuru),
                 QStringLiteral("Nuuru (Soybooru)"));
    }

    void translateDropsRatingAndSort() {
        QCOMPARE(RemotePosts::translateSearchTags(
                     {QStringLiteral("chudjak"),
                      QStringLiteral("rating:explicit"),
                      QStringLiteral("sort:bogus")},
                     BooruEngine::Nuuru),
                 QStringList({QStringLiteral("chudjak")}));
        // Native sort: spellings survive server-side.
        QCOMPARE(RemotePosts::translateSearchTags(
                     {QStringLiteral("chudjak"),
                      QStringLiteral("sort:score_desc")},
                     BooruEngine::Nuuru),
                 QStringList({QStringLiteral("chudjak"),
                              QStringLiteral("sort:score_desc")}));
    }

    void sortMapsServerSide() {
        QCOMPARE(RemotePosts::sortSearchTag(QStringLiteral("id-desc"),
                                            BooruEngine::Nuuru),
                 QStringLiteral("sort:id_desc"));
        QCOMPARE(RemotePosts::sortSearchTag(QStringLiteral("id-asc"),
                                            BooruEngine::Nuuru),
                 QStringLiteral("sort:id_asc"));
        QCOMPARE(RemotePosts::sortSearchTag(QStringLiteral("score-desc"),
                                            BooruEngine::Nuuru),
                 QStringLiteral("sort:score_desc"));
        QCOMPARE(RemotePosts::sortSearchTag(QStringLiteral("score-asc"),
                                            BooruEngine::Nuuru),
                 QStringLiteral("sort:score_asc"));
        QCOMPARE(RemotePosts::sortSearchTag(QStringLiteral("random"),
                                            BooruEngine::Nuuru),
                 QStringLiteral("sort:random"));
        QCOMPARE(RemotePosts::sortSearchTag(QStringLiteral("added"),
                                            BooruEngine::Nuuru),
                 QStringLiteral(""));
    }

    void tagsParsing() {
        // Assumed shape (tag endpoints need a browser session live):
        // [{name, category, postCount}].
        const auto out = TagSuggester::parse(
            BooruEngine::Nuuru,
            QByteArray(R"([{"name":"chudjak","category":"variant","postCount":99},)"
                       R"({"name":"cobson","category":null,"postCount":5}])"));
        QCOMPARE(out.size(), 2);
        QCOMPARE(out.at(0).tag, QStringLiteral("chudjak"));
        QCOMPARE(out.at(0).bucket, QStringLiteral("character")); // variant fold
        QCOMPARE(out.at(1).bucket, QStringLiteral("general")); // uncategorized
    }
};

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    TestNuuru tc;
    return QTest::qExec(&tc, argc, argv);
}
#include "test_nuuru.moc"
