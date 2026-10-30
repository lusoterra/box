// HyBooru engine: search/detail/tag shapes, URL schemes, tag translation.
// Fixtures are trimmed captures from a live instance (funmaker).
#include "booruengine.h"
#include "tagsuggest.h"

#include <QCoreApplication>
#include <QUrlQuery>
#include <QtTest>

namespace {
// Two-post search page (pageSize echoes as a string; summaries carry no
// tags or dimensions).
QByteArray searchFixture() {
    return QByteArray(
        R"({"posts":[{"id":19099,"sha256":"27b7beaf0de2ed8a51c1fa42af6e0826ae44ceff0b0ddeca270cb6b7c1fa1b2c","hash":"27b7beaf","md5":"b8bc6b05a7d09bb12cffd900b9fbd556","size":2470667,"mime":1,"posted":"2026-09-21T18:35:08.000Z","extension":".jpg"},)"
        R"({"id":19100,"sha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","md5":"cccccccccccccccccccccccccccccccc","size":9911,"mime":14,"posted":"2026-09-21T18:35:00.000Z","extension":".mp4"}],"pageSize":"2","total":7231})");
}
// Detail object (tags trimmed to a few namespaced entries).
QByteArray detailFixture() {
    return QByteArray(
        R"({"id":19099,"sha256":"27b7beaf0de2ed8a51c1fa42af6e0826ae44ceff0b0ddeca270cb6b7c1fa1b2c","hash":"27b7beaf","md5":"b8bc6b05a7d09bb12cffd900b9fbd556","size":2470667,"width":1191,"height":1684,"duration":null,"rating":0.6,"mime":1,"posted":"2026-09-21T18:35:08.000Z",)"
        R"("tags":{"character:hatsune_miku":6490,"creator:himukai_aoi":6,"series:vocaloid":6492,"medium:speech_bubble":63,"meta:translated":94,"twintails":3629},"sources":["https://cdn.donmai.us/original/b8/bc/b8bc6b05a7d09bb12cffd900b9fbd556.jpg","https://danbooru.donmai.us/posts/6160901"],"extension":".jpg"})");
}
QByteArray tagsFixture() {
    return QByteArray(R"({"tags":{"miku":346,"title:miku":91},"total":2,"pageSize":50})");
}
const QString kBase = QStringLiteral("https://booru.funmaker.moe");
} // namespace

class TestHybooru : public QObject {
    Q_OBJECT
private slots:
    void searchParsing() {
        const BooruEngineOps *ops = engineOps(BooruEngine::Hybooru);
        const SearchResult r = ops->parseSearch(
            QStringLiteral("funmaker"),
            {BooruEngine::Hybooru, kBase, {}}, searchFixture());
        QVERIFY(r.ok());
        QCOMPARE(r.posts.size(), 2);
        const Post still = r.posts.at(0);
        QCOMPARE(still.id, 19099LL);
        QCOMPARE(still.ext, QStringLiteral("jpg")); // dot stripped
        QCOMPARE(still.fileUrl,
                 kBase + QStringLiteral("/files/f27b7beaf0de2ed8a51c1fa42af6e0826ae44ceff0b0ddeca270cb6b7c1fa1b2c.jpg"));
        QCOMPARE(still.previewUrl,
                 kBase + QStringLiteral("/files/t27b7beaf0de2ed8a51c1fa42af6e0826ae44ceff0b0ddeca270cb6b7c1fa1b2c.thumbnail"));
        QCOMPARE(still.postUrl,
                 kBase + QStringLiteral("/posts/19099"));
        QVERIFY(still.tags.isEmpty()); // summaries are tagless by design
        QCOMPARE(still.width, 0);
        QCOMPARE(still.rating, QStringLiteral("general")); // stars, not content
        QVERIFY(still.isRemote && still.hasMeta);
        // md5 survives for download naming (hashes=true requested).
        QCOMPARE(still.rawJson.value(QStringLiteral("md5")).toString(),
                 QStringLiteral("b8bc6b05a7d09bb12cffd900b9fbd556"));
        // Video mime still routes on extension.
        const Post clip = r.posts.at(1);
        QCOMPARE(clip.ext, QStringLiteral("mp4"));
        QVERIFY(clip.fileUrl.endsWith(QStringLiteral(".mp4")));
    }

    void detailParsing() {
        const Post p = RemotePosts::fromHybooru(
            QJsonDocument::fromJson(detailFixture()).object(), kBase);
        QCOMPARE(p.id, 19099LL);
        QCOMPARE(p.width, 1191);
        QCOMPARE(p.height, 1684);
        QCOMPARE(p.source,
                 QStringLiteral("https://cdn.donmai.us/original/b8/bc/b8bc6b05a7d09bb12cffd900b9fbd556.jpg"));
        QCOMPARE(p.rating, QStringLiteral("general"));
        // Namespaces fold into buckets...
        QVERIFY(p.tagsArtist.split(u' ').contains(QStringLiteral("himukai_aoi")));
        QVERIFY(p.tagsCharacter.split(u' ').contains(QStringLiteral("hatsune_miku")));
        QVERIFY(p.tagsCopyright.split(u' ').contains(QStringLiteral("vocaloid")));
        QVERIFY(p.tagsGeneral.split(u' ').contains(QStringLiteral("twintails")));
        QVERIFY(p.tagsMeta.split(u' ').contains(QStringLiteral("translated")));
        QVERIFY(p.tagsMeta.split(u' ').contains(QStringLiteral("speech_bubble")));
        // Folded namespaces are not duplicated as custom groups.
        QVERIFY(p.customTags.isEmpty());
        QVERIFY(!p.tags.isEmpty());
    }

    void md5PopulatedForTwinCollapse() {
        // finishRemote lifts a validated md5 onto the post for
        // cross-entry twin collapse (md5DedupeKey).
        const QJsonObject o =
            QJsonDocument::fromJson(detailFixture()).object();
        const Post p = RemotePosts::fromHybooru(o, kBase);
        QCOMPARE(p.md5, QStringLiteral("b8bc6b05a7d09bb12cffd900b9fbd556"));
        QVERIFY(!md5DedupeKey(p).isEmpty());
    }

    void urls() {
        const BooruEngineOps *ops = engineOps(BooruEngine::Hybooru);
        const BooruEngineOps::Context ctx{BooruEngine::Hybooru, kBase, {}};
        // box pages are 1-indexed; the API is 0-indexed.
        const QUrlQuery first(QUrl(ops->searchUrl(
            ctx, {QStringLiteral("hatsune_miku")}, 1, 40)).query());
        QCOMPARE(first.queryItemValue(QStringLiteral("page")), QStringLiteral("0"));
        QCOMPARE(first.queryItemValue(QStringLiteral("query")),
                 QStringLiteral("hatsune_miku"));
        QCOMPARE(first.queryItemValue(QStringLiteral("pageSize")), QStringLiteral("40"));
        QCOMPARE(first.queryItemValue(QStringLiteral("hashes")), QStringLiteral("true"));
        const QUrlQuery third(QUrl(ops->searchUrl(ctx, {}, 3, 40)).query());
        QCOMPARE(third.queryItemValue(QStringLiteral("page")), QStringLiteral("2"));
        QCOMPARE(ops->refreshUrl(ctx, 19099).toString(),
                 kBase + QStringLiteral("/api/post/19099"));
        QCOMPARE(RemotePosts::engineKey(BooruEngine::Hybooru),
                 QStringLiteral("hybooru"));
        QCOMPARE(RemotePosts::engineForKey(QStringLiteral("hybooru")),
                 BooruEngine::Hybooru);
        QCOMPARE(RemotePosts::engineLabel(BooruEngine::Hybooru),
                 QStringLiteral("HyBooru"));
    }

    void translateDropsRatingAndSort() {
        // rating: is a star filter on HyBooru and sort is client-side:
        // both must vanish rather than mis-filter.
        QCOMPARE(RemotePosts::translateSearchTags(
                     {QStringLiteral("hatsune_miku"),
                      QStringLiteral("rating:general"),
                      QStringLiteral("sort:score")},
                     BooruEngine::Hybooru),
                 QStringList({QStringLiteral("hatsune_miku")}));
        QCOMPARE(RemotePosts::sortSearchTag(QStringLiteral("score-desc"),
                                           BooruEngine::Hybooru),
                 QString());
    }

    void tagsParsing() {
        const auto out =
            TagSuggester::parse(BooruEngine::Hybooru, tagsFixture());
        QCOMPARE(out.size(), 2);
        // Namespaces strip to the bare tag; buckets follow the mapping.
        QCOMPARE(out.at(0).tag, QStringLiteral("miku"));
        QCOMPARE(out.at(0).count, 346);
        QCOMPARE(out.at(1).tag, QStringLiteral("miku"));
        QCOMPARE(out.at(1).bucket, QStringLiteral("meta")); // was title:miku
    }
};

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    TestHybooru tc;
    return QTest::qExec(&tc, argc, argv);
}
#include "test_hybooru.moc"
