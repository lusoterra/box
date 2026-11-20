// Philomena (Derpibooru, v1 API) + BooruOnRails (Twibooru, v3 API) fixtures.
#include <QtTest>
#include <QJsonObject>

#include "booruengine.h"
#include "remotesource.h"
#include "tagsuggest.h"

class TestPhilomena : public QObject {
    Q_OBJECT
private slots:
    void derpibooruPost() {
        const QByteArray body = QByteArrayLiteral(
            "{\"image\":{\"id\":3926792,\"score\":3,\"width\":1836,\"height\":2628,"
            "\"format\":\"png\",\"created_at\":\"2026-11-05T17:34:08Z\","
            "\"source_url\":\"https://example.com/src\","
            "\"view_url\":\"https://derpicdn.net/img/view/2026/11/5/3926792.png\","
            "\"representations\":{\"full\":\"https://derpicdn.net/img/view/2026/11/5/3926792.png\","
            "\"large\":\"https://derpicdn.net/img/2026/11/5/3926792/large.png\","
            "\"thumb\":\"https://derpicdn.net/img/2026/11/5/3926792/thumb.png\"},"
            "\"tags\":[\"safe\",\"pony\",\"oc\",\"artist:mediasmile666\",\"character:test\"]}}");
        bool ok = false;
        const QJsonObject raw = engineOps(BooruEngine::Philomena)->parseSingle(body, ok);
        QVERIFY(ok);
        const Post p = RemotePosts::fromPhilomenaFamily(BooruEngine::Philomena, raw, QStringLiteral("https://derpibooru.org"));
        QCOMPARE(p.id, qint64(3926792));
        QCOMPARE(p.score, 3);
        QCOMPARE(p.rating, QStringLiteral("safe"));
        QCOMPARE(p.width, 1836);
        QCOMPARE(p.height, 2628);
        QVERIFY(p.fileUrl.contains(QStringLiteral("derpicdn")));
        QVERIFY(p.previewUrl.contains(QStringLiteral("thumb")));
        QVERIFY(p.tagsArtist.contains(QStringLiteral("artist:mediasmile666")));
        QVERIFY(p.tagsCharacter.contains(QStringLiteral("character:test")));
        QVERIFY(p.tags.contains(QStringLiteral("pony")));
        QCOMPARE(p.postUrl, QStringLiteral("https://derpibooru.org/images/3926792"));
    }

    void derpibooruSearch() {
        const QByteArray body = R"({"total":2751039,"images":[{"id":1,"score":0,"width":10,"height":10,"format":"png","tags":["safe"],"representations":{"full":"https://x/f.png","thumb":"https://x/t.png"}}]})";
        const SearchResult r = engineOps(BooruEngine::Philomena)->parseSearch(
            QStringLiteral("k"), {BooruEngine::Philomena, QStringLiteral("https://derpibooru.org"), {}}, body);
        QVERIFY(r.error.isEmpty());
        QCOMPARE(r.posts.size(), 1);
        QCOMPARE(r.posts.first().id, qint64(1));
    }

    void twibooruPost() {
        const QByteArray body = QByteArrayLiteral(
            "{\"post\":{\"id\":3889209,\"score\":2,\"width\":1206,\"height\":2426,"
            "\"format\":\"png\",\"view_url\":\"https://cdn.twibooru.org/img/2026/11/5/3889209/full.png\","
            "\"representations\":{\"full\":\"https://cdn.twibooru.org/img/2026/11/5/3889209/full.png\","
            "\"thumb\":\"https://cdn.twibooru.org/img/2026/11/5/3889209/thumb.webp\"},"
            "\"tags\":[\"safe\",\"oc\",\"artist:ask-luciavampire\",\"oc:rosa nyx\"]}}");
        bool ok = false;
        const QJsonObject raw = engineOps(BooruEngine::BooruOnRails)->parseSingle(body, ok);
        QVERIFY(ok);
        const Post p = RemotePosts::fromPhilomenaFamily(BooruEngine::BooruOnRails, raw, QStringLiteral("https://twibooru.org"));
        QCOMPARE(p.id, qint64(3889209));
        QCOMPARE(p.rating, QStringLiteral("safe"));
        QVERIFY(p.tagsArtist.contains(QStringLiteral("artist:ask-luciavampire")));
        QVERIFY(p.tagsCharacter.contains(QStringLiteral("oc:rosa nyx")));
        QCOMPARE(p.postUrl, QStringLiteral("https://twibooru.org/posts/3889209"));
    }

    void twibooruSearch() {
        const QByteArray body = R"({"total":3133581,"posts":[{"id":5,"score":0,"width":1,"height":1,"format":"png","tags":["safe"],"representations":{"full":"https://x/f.png","thumb":"https://x/t.png"},"view_url":"https://x/f.png"}]})";
        const SearchResult r = engineOps(BooruEngine::BooruOnRails)->parseSearch(
            QStringLiteral("k"), {BooruEngine::BooruOnRails, QStringLiteral("https://twibooru.org"), {}}, body);
        QVERIFY(r.error.isEmpty());
        QCOMPARE(r.posts.size(), 1);
    }

    void tagSuggest() {
        const QByteArray derpi = QByteArrayLiteral(
            "{\"tags\":[{\"name\":\"artist:test\",\"images\":42,\"category\":\"artist\"},"
            "{\"name\":\"safe\",\"images\":100,\"category\":\"rating\"}]}");
        auto ds = TagSuggester::parse(BooruEngine::Philomena, derpi);
        QCOMPARE(ds.size(), 2);
        QCOMPARE(ds[0].tag, QStringLiteral("artist:test"));
        QCOMPARE(ds[0].count, 42);
        const QByteArray twi = QByteArrayLiteral(
            "{\"tags\":[{\"name\":\"oc:foo\",\"posts\":7,\"category\":\"oc\"}]}");
        auto ts = TagSuggester::parse(BooruEngine::BooruOnRails, twi);
        QCOMPARE(ts.size(), 1);
        QCOMPARE(ts[0].tag, QStringLiteral("oc:foo"));
        QCOMPARE(ts[0].bucket, QStringLiteral("character"));
    }

    void engineMeta() {
        QCOMPARE(RemotePosts::engineKey(BooruEngine::Philomena),
                 QStringLiteral("philomena"));
        QCOMPARE(RemotePosts::engineKey(BooruEngine::BooruOnRails),
                 QStringLiteral("booru-on-rails"));
        QCOMPARE(RemotePosts::engineForKey(QStringLiteral("philomena")),
                 BooruEngine::Philomena);
        QCOMPARE(RemotePosts::engineForKey(QStringLiteral("booru-on-rails")),
                 BooruEngine::BooruOnRails);
        // Pre-rename keys keep resolving (saved sites + favourites).
        QCOMPARE(RemotePosts::engineForKey(QStringLiteral("derpibooru")),
                 BooruEngine::Philomena);
        QCOMPARE(RemotePosts::engineForKey(QStringLiteral("twibooru")),
                 BooruEngine::BooruOnRails);
        QCOMPARE(RemotePosts::shortFor(
                     RemotePosts::engineLabel(BooruEngine::Philomena)),
                 QStringLiteral("PHI"));
        QCOMPARE(RemotePosts::shortFor(
                     RemotePosts::engineLabel(BooruEngine::BooruOnRails)),
                 QStringLiteral("BOO"));
    }

    void familyShapes() {
        // One implementation, two shapes: only paths/envelope keys differ.
        QCOMPARE(RemotePosts::isPhilomenaFamily(BooruEngine::Philomena), true);
        QCOMPARE(RemotePosts::isPhilomenaFamily(BooruEngine::BooruOnRails), true);
        QCOMPARE(RemotePosts::isPhilomenaFamily(BooruEngine::Danbooru), false);
        QCOMPARE(RemotePosts::philomenaApiRoot(BooruEngine::Philomena),
                 QStringLiteral("/api/v1/json"));
        QCOMPARE(RemotePosts::philomenaApiRoot(BooruEngine::BooruOnRails),
                 QStringLiteral("/api/v3"));
        QCOMPARE(RemotePosts::philomenaListKey(BooruEngine::Philomena),
                 QStringLiteral("images"));
        QCOMPARE(RemotePosts::philomenaListKey(BooruEngine::BooruOnRails),
                 QStringLiteral("posts"));
        QCOMPARE(RemotePosts::philomenaItemKey(BooruEngine::Philomena),
                 QStringLiteral("image"));
        QCOMPARE(RemotePosts::philomenaItemKey(BooruEngine::BooruOnRails),
                 QStringLiteral("post"));

        // String-key post pages keep both the new and the legacy keys.
        QCOMPARE(RemotePosts::postPageUrl(QStringLiteral("philomena"),
                                          QStringLiteral("https://derpibooru.org"), 7),
                 QStringLiteral("https://derpibooru.org/images/7"));
        QCOMPARE(RemotePosts::postPageUrl(QStringLiteral("booru-on-rails"),
                                          QStringLiteral("https://twibooru.org"), 8),
                 QStringLiteral("https://twibooru.org/posts/8"));
        QCOMPARE(RemotePosts::postPageUrl(QStringLiteral("twibooru"),
                                          QStringLiteral("https://twibooru.org"), 8),
                 QStringLiteral("https://twibooru.org/posts/8"));

        // Third-party instances are named after their host, not Derpibooru.
        const QJsonObject o{{QStringLiteral("id"), 3}};
        QCOMPARE(RemotePosts::fromPhilomenaFamily(
                     BooruEngine::Philomena, o,
                     QStringLiteral("https://ponybooru.org")).siteName,
                 QStringLiteral("ponybooru.org"));
        QCOMPARE(RemotePosts::fromPhilomenaFamily(
                     BooruEngine::Philomena, o,
                     QStringLiteral("https://derpibooru.org")).siteName,
                 QStringLiteral("Derpibooru"));
    }

    void urls() {
        const BooruEngineOps::Context dctx{BooruEngine::Philomena,
                                           QStringLiteral("https://derpibooru.org"), {}};
        const QUrl ds = engineOps(BooruEngine::Philomena)
                            ->searchUrl(dctx, {QStringLiteral("safe")}, 2, 25);
        QVERIFY(ds.toString().contains(QStringLiteral("/api/v1/json/search/images")));
        QVERIFY(ds.toString().contains(QStringLiteral("page=2")));
        const QUrl dr = engineOps(BooruEngine::Philomena)->refreshUrl(dctx, 123);
        QCOMPARE(dr.toString(),
                 QStringLiteral("https://derpibooru.org/api/v1/json/images/123"));

        const BooruEngineOps::Context tctx{BooruEngine::BooruOnRails,
                                           QStringLiteral("https://twibooru.org"), {}};
        const QUrl ts = engineOps(BooruEngine::BooruOnRails)
                            ->searchUrl(tctx, {QStringLiteral("safe")}, 1, 25);
        QVERIFY(ts.toString().contains(QStringLiteral("/api/v3/search/posts")));
        const QUrl tr = engineOps(BooruEngine::BooruOnRails)->refreshUrl(tctx, 456);
        QCOMPARE(tr.toString(), QStringLiteral("https://twibooru.org/api/v3/posts/456"));

        // Post-page URLs (postPageUrlFor, src/remote/remotesource.cpp:973).
        BooruSite derpi;
        derpi.engine = BooruEngine::Philomena;
        derpi.baseUrl = QStringLiteral("https://derpibooru.org");
        QCOMPARE(RemotePosts::postPageUrlFor(derpi, 7),
                 QStringLiteral("https://derpibooru.org/images/7"));
        BooruSite twi;
        twi.engine = BooruEngine::BooruOnRails;
        twi.baseUrl = QStringLiteral("https://twibooru.org");
        QCOMPARE(RemotePosts::postPageUrlFor(twi, 8),
                 QStringLiteral("https://twibooru.org/posts/8"));

        // String-key post URLs (src/engines/booruengine.cpp:666).
        QCOMPARE(RemotePosts::postPageUrl(QStringLiteral("derpibooru"),
                                          QStringLiteral("https://derpibooru.org"), 7),
                 QStringLiteral("https://derpibooru.org/images/7"));
        QCOMPARE(RemotePosts::postPageUrl(QStringLiteral("twibooru"),
                                          QStringLiteral("https://twibooru.org"), 8),
                 QStringLiteral("https://twibooru.org/posts/8"));

    }

    void ratingMapping() {
        // Philomena rating tags -> app rating buckets (src/engines/engine_philomena.cpp).
        auto parseD = [](const char *tags) {
            const QByteArray body = QByteArrayLiteral("{\"image\":{\"id\":1,\"width\":1,\"height\":1,"
                                                      "\"format\":\"png\",\"representations\":{\"full\":\"https://x/f.png\","
                                                      "\"thumb\":\"https://x/t.png\"},\"tags\":") +
                                    QByteArray(tags) + QByteArrayLiteral("}}");
            bool ok = false;
            return RemotePosts::fromPhilomenaFamily(BooruEngine::Philomena,
                engineOps(BooruEngine::Philomena)->parseSingle(body, ok),
                QStringLiteral("https://derpibooru.org"));
        };
        Post p = parseD("[\"suggestive\",\"grimdark\",\"artist:a\"]");
        QCOMPARE(p.rating, QStringLiteral("questionable"));
        QVERIFY(p.tagsMeta.contains(QStringLiteral("grimdark")));
        QVERIFY(p.tagsArtist.contains(QStringLiteral("artist:a")));
        QCOMPARE(parseD("[\"explicit\"]").rating, QStringLiteral("explicit"));

        const QByteArray tBody = QByteArrayLiteral(
            "{\"post\":{\"id\":2,\"width\":1,\"height\":1,\"format\":\"png\","
            "\"view_url\":\"https://x/f.png\",\"representations\":{\"full\":\"https://x/f.png\",\"thumb\":\"https://x/t.png\"},"
            "\"tags\":[\"questionable\",\"semi-grimdark\"]}}");
        bool ok = false;
        const Post tp = RemotePosts::fromPhilomenaFamily(BooruEngine::BooruOnRails,
            engineOps(BooruEngine::BooruOnRails)->parseSingle(tBody, ok),
            QStringLiteral("https://twibooru.org"));
        QVERIFY(ok);
        QCOMPARE(tp.rating, QStringLiteral("questionable"));
        QVERIFY(tp.tagsMeta.contains(QStringLiteral("semi-grimdark")));
    }

    void translateMetatags() {
        // Philomena engines strip unverified rating/sort/order metatags
        // client-side (src/engines/booruengine.cpp:445-465). Negated
        // metatags keep their '-' prefix in `head`, so they are not
        // stripped by the current Philomena/Hybooru branch (behavior as-is).
        const QStringList in{QStringLiteral("rating:safe"),
                             QStringLiteral("sort:score"),
                             QStringLiteral("order:score"),
                             QStringLiteral("pony"),
                             QStringLiteral("-artist:foo")};
        const QStringList want{QStringLiteral("pony"),
                               QStringLiteral("-artist:foo")};
        QCOMPARE(RemotePosts::translateSearchTags(in, BooruEngine::Philomena), want);
        QCOMPARE(RemotePosts::translateSearchTags(in, BooruEngine::BooruOnRails), want);
        // Document current behavior for negated metatags.
        QCOMPARE(RemotePosts::translateSearchTags(
                     {QStringLiteral("-rating:safe"), QStringLiteral("pony")},
                     BooruEngine::Philomena),
                 QStringList({QStringLiteral("-rating:safe"),
                              QStringLiteral("pony")}));
    }

    void ratingMappingGaps() {
        auto parseD = [](const char *tags) {
            const QByteArray body = QByteArrayLiteral("{\"image\":{\"id\":1,\"width\":1,\"height\":1,"
                                                      "\"format\":\"png\",\"representations\":{\"full\":\"https://x/f.png\","
                                                      "\"thumb\":\"https://x/t.png\"},\"tags\":") +
                                    QByteArray(tags) + QByteArrayLiteral("}}");
            bool ok = false;
            return RemotePosts::fromPhilomenaFamily(BooruEngine::Philomena,
                engineOps(BooruEngine::Philomena)->parseSingle(body, ok),
                QStringLiteral("https://derpibooru.org"));
        };
        // grimdark upgrades safe to questionable
        QCOMPARE(parseD("[\"safe\",\"semi-grimdark\"]").rating,
                 QStringLiteral("questionable"));
        // safe alone -> safe
        QCOMPARE(parseD("[\"safe\"]").rating, QStringLiteral("safe"));
        // no rating tags -> general
        QCOMPARE(parseD("[\"artist:a\"]").rating, QStringLiteral("general"));
    }

    void malformedAndMinimal() {
        const BooruEngineOps::Context dctx{BooruEngine::Philomena,
                                           QStringLiteral("https://derpibooru.org"), {}};
        const SearchResult bad = engineOps(BooruEngine::Philomena)
                                     ->parseSearch(QStringLiteral("k"), dctx, "not json");
        QVERIFY(!bad.error.isEmpty());
        QVERIFY(bad.posts.isEmpty());
        bool ok = true;
        QVERIFY(engineOps(BooruEngine::Philomena)->parseSingle("nope", ok).isEmpty());
        QVERIFY(!ok);
        ok = true;
        QVERIFY(engineOps(BooruEngine::BooruOnRails)->parseSingle("nope", ok).isEmpty());
        QVERIFY(!ok);

        const QByteArray minBody = QByteArrayLiteral(
            "{\"image\":{\"id\":9,\"width\":0,\"height\":0,"
            "\"representations\":{},\"tags\":[]}}");
        ok = false;
        const QJsonObject raw = engineOps(BooruEngine::Philomena)->parseSingle(minBody, ok);
        QVERIFY(ok);
        const Post p = RemotePosts::fromPhilomenaFamily(BooruEngine::Philomena, raw, QStringLiteral("https://derpibooru.org"));
        QCOMPARE(p.id, qint64(9));
        QVERIFY(p.isRemote);
        QCOMPARE(p.postUrl, QStringLiteral("https://derpibooru.org/images/9"));
    }
};

QTEST_MAIN(TestPhilomena)
#include "test_philomena.moc"
