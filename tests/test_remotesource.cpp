// Remote booru clients: site registry, per-engine API-object mapping,
// filename/URL helpers, single-post parsing, refresh-target resolution.
// Pure functions only (remotesource.cpp); no network.
#include "remotesource.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QCoreApplication>
#include <QNetworkCookie>
#include <QNetworkRequest>
#include <QUrl>
#include <QtTest>

namespace {
QJsonObject danbooruObj() {
    QJsonObject o;
    o[QStringLiteral("id")] = 100;
    QJsonObject score;
    score[QStringLiteral("total")] = 8;
    o[QStringLiteral("score")] = score;
    o[QStringLiteral("rating")] = QStringLiteral("g");
    o[QStringLiteral("tag_string_artist")] = QStringLiteral("an_artist");
    o[QStringLiteral("tag_string_general")] = QStringLiteral("1girl sky");
    o[QStringLiteral("image_width")] = 800;
    o[QStringLiteral("image_height")] = 600;
    o[QStringLiteral("source")] = QStringLiteral("https://src.example");
    o[QStringLiteral("created_at")] = QStringLiteral("2024-05-01T00:00:00Z");
    o[QStringLiteral("file_url")] = QStringLiteral("https://cdn.donmai.us/a/b.jpg");
    o[QStringLiteral("large_file_url")] = QStringLiteral("https://cdn.donmai.us/a/b_large.jpg");
    o[QStringLiteral("preview_file_url")] = QStringLiteral("https://cdn.donmai.us/a/b_prev.jpg");
    return o;
}
} // namespace

class TestRemoteSource : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        // Registry-backed lookups resolve through this in these tests.
        RemoteSource::setConfiguredBoorus(RemoteSource::presetBoorus());
    }
    void cleanupTestCase() { RemoteSource::setConfiguredBoorus({}); }

    void registrySane() {
        // New users ship Safebooru-only (SFW default); the registry is
        // still what configured boorus are pushed into.
        const QList<BooruSite> sites = RemoteSource::presetBoorus();
        QCOMPARE(sites.size(), 1);
        QSet<QString> keys, shorts;
        for (const BooruSite &s : sites) {
            QVERIFY(!s.key.isEmpty() && !s.label.isEmpty());
            QVERIFY(!keys.contains(s.key));
            QVERIFY(!shorts.contains(s.shortName));
            keys.insert(s.key);
            shorts.insert(s.shortName);
            QVERIFY(RemoteSource::siteFor(s.key) != nullptr);
        }
        QCOMPARE(sites.first().key, QStringLiteral("safebooru"));
        QVERIFY(!sites.first().baseUrl.isEmpty());
        QVERIFY(RemoteSource::siteFor(QStringLiteral("nope")) == nullptr);
    }

    void badgeColorDefaults() {
        using RemotePosts::defaultBadgeColor;
        QCOMPARE(defaultBadgeColor(QStringLiteral("https://gelbooru.com"),
                                   BooruEngine::Gelbooru),
                 QStringLiteral("#0870fa"));
        QCOMPARE(defaultBadgeColor(QStringLiteral("https://rule34.xxx"),
                                   BooruEngine::Gelbooru),
                 QStringLiteral("#55aa00"));
        QCOMPARE(defaultBadgeColor(QStringLiteral("https://aibooru.online"),
                                   BooruEngine::Danbooru),
                 QStringLiteral("#44cc00"));
        QCOMPARE(defaultBadgeColor(QStringLiteral("https://danbooru.donmai.us"),
                                   BooruEngine::Danbooru),
                 QStringLiteral("#daac82"));
        QCOMPARE(defaultBadgeColor(QStringLiteral("https://safebooru.org"),
                                   BooruEngine::Gelbooru),
                 QStringLiteral("#daac82"));
        QCOMPARE(defaultBadgeColor(QStringLiteral("https://shimmie.example"),
                                   BooruEngine::Shimmie),
                 QStringLiteral("#7f7f7f"));
        // Unknown hosts keep the neutral blue; shimmie rule needs the
        // engine (a danbooru on the same host is not a shimmie).
        QCOMPARE(defaultBadgeColor(QStringLiteral("https://unknown.example"),
                                   BooruEngine::Danbooru),
                 QStringLiteral("#3a7ec2"));
        QCOMPARE(defaultBadgeColor(QStringLiteral("https://shimmie.example"),
                                   BooruEngine::Danbooru),
                 QStringLiteral("#3a7ec2"));
        // The shipped preset already wears its house color.
        QCOMPARE(RemoteSource::presetBoorus().first().badgeColor,
                 QStringLiteral("#daac82"));
    }

    void configuredShadowsPresets() {
        BooruSite custom;
        custom.key = QStringLiteral("gelbooru");
        custom.label = QStringLiteral("My Gelbooru");
        custom.shortName = QStringLiteral("MGL");
        custom.baseUrl = QStringLiteral("https://gel.example");
        custom.engine = BooruEngine::Gelbooru;
        custom.user = QStringLiteral("u");
        custom.secret = QStringLiteral("s");
        RemoteSource::setConfiguredBoorus({custom});
        const BooruSite *found = RemoteSource::siteFor(QStringLiteral("gelbooru"));
        QVERIFY(found != nullptr);
        QCOMPARE(found->baseUrl, QStringLiteral("https://gel.example"));
        QCOMPARE(RemotePosts::keyForLabel(QStringLiteral("My Gelbooru")),
                 QStringLiteral("gelbooru"));
        // Preset labels still resolve when not shadowed.
        QCOMPARE(RemotePosts::keyForLabel(QStringLiteral("Danbooru")),
                 QStringLiteral("danbooru"));
        RemoteSource::setConfiguredBoorus(RemoteSource::presetBoorus());
    }

    void mediaRefererPrefersPostPage() {
        // The post page itself satisfies hotlink checks with no registry
        // lookup, so renamed/custom labels work too.
        Post p;
        p.siteName = QStringLiteral("Some Custom Label");
        p.postUrl = QStringLiteral("https://rule34.xxx/index.php?page=post&s=view&id=5");
        QCOMPARE(RemotePosts::mediaRefererFor(p),
                 QStringLiteral("https://rule34.xxx/"));
        // No page link: falls back to the configured base URL.
        Post q;
        q.siteName = QStringLiteral("Gelbooru");
        BooruSite gel;
        gel.key = QStringLiteral("gelbooru");
        gel.label = QStringLiteral("Gelbooru");
        gel.baseUrl = QStringLiteral("https://gelbooru.com");
        gel.engine = BooruEngine::Gelbooru;
        RemoteSource::setConfiguredBoorus({gel});
        QCOMPARE(RemotePosts::mediaRefererFor(q),
                 QStringLiteral("https://gelbooru.com/"));
        // Unknown everywhere: empty (callers then send no Referer).
        Post r;
        r.siteName = QStringLiteral("Nope");
        QVERIFY(RemotePosts::mediaRefererFor(r).isEmpty());
        RemoteSource::setConfiguredBoorus(RemoteSource::presetBoorus());
    }

    void entryForPostResolvesDownloads() {
        BooruSite dan;
        dan.key = QStringLiteral("danbooru");
        dan.label = QStringLiteral("Danbooru");
        dan.shortName = QStringLiteral("DAN");
        dan.baseUrl = QStringLiteral("https://danbooru.donmai.us");
        dan.engine = BooruEngine::Danbooru;
        RemoteSource::setConfiguredBoorus({dan});
        // Remote posts resolve via site name...
        Post r;
        r.isRemote = true;
        r.siteName = QStringLiteral("Danbooru");
        QCOMPARE(RemotePosts::entryForPost(r)->key, QStringLiteral("danbooru"));
        // ...and downloaded rows resolve via their host folder.
        Post l;
        l.isRemote = false;
        l.category = QStringLiteral("danbooru.donmai.us");
        l.absFile = QStringLiteral("/dl/danbooru.donmai.us/x.jpg");
        const BooruSite *e = RemotePosts::entryForPost(l);
        QVERIFY(e != nullptr);
        QCOMPARE(e->key, QStringLiteral("danbooru"));
        // Legacy label folders resolve too.
        l.category = QStringLiteral("Danbooru");
        QCOMPARE(RemotePosts::entryForPost(l)->key, QStringLiteral("danbooru"));
        // Engine-shaped post URLs for downloads missing one.
        QCOMPARE(RemotePosts::postPageUrlFor(*e, 123),
                 QStringLiteral("https://danbooru.donmai.us/posts/123"));
        QVERIFY(RemotePosts::postPageUrlFor(*e, 0).isEmpty());
        // Strays and plain folders resolve to nothing.
        Post stray;
        stray.isRemote = false;
        stray.category = QStringLiteral("_Stray");
        QVERIFY(RemotePosts::entryForPost(stray) == nullptr);
        Post lib;
        lib.isRemote = false;
        lib.category = QStringLiteral("vacation");
        QVERIFY(RemotePosts::entryForPost(lib) == nullptr);
        RemoteSource::setConfiguredBoorus(RemoteSource::presetBoorus());
    }

    void webDownloadRouting() {
        // Walled entries route by base host, not file host: CDN media
        // (cdn.*) rides the browser stack like same-host media does.
        BooruSite atf;
        atf.key = QStringLiteral("atf");
        atf.label = QStringLiteral("ATF");
        atf.baseUrl = QStringLiteral("https://booru.allthefallen.moe");
        atf.engine = BooruEngine::Shimmie;
        BooruSite gel;
        gel.key = QStringLiteral("gelbooru");
        gel.label = QStringLiteral("Gelbooru");
        gel.baseUrl = QStringLiteral("https://gelbooru.com");
        gel.engine = BooruEngine::Gelbooru;
        RemoteSource::setConfiguredBoorus({atf, gel});
        Post cdn;
        cdn.isRemote = true;
        cdn.siteKey = QStringLiteral("atf");
        cdn.siteName = QStringLiteral("ATF");
        cdn.fileUrl = QStringLiteral("https://cdn.allthefallen.moe/x.jpg");
        QVERIFY(RemotePosts::needsWebDownload(cdn));
        Post direct = cdn;
        direct.fileUrl =
            QStringLiteral("https://booru.allthefallen.moe/x.jpg");
        QVERIFY(RemotePosts::needsWebDownload(direct));
        // Label-only rows (favourites) resolve the same way.
        Post labelled;
        labelled.isRemote = true;
        labelled.siteName = QStringLiteral("ATF");
        labelled.fileUrl = QStringLiteral("https://cdn.allthefallen.moe/y.jpg");
        QVERIFY(RemotePosts::needsWebDownload(labelled));
        // Plain sites stay plain, even with CDN file hosts.
        Post plain;
        plain.isRemote = true;
        plain.siteKey = QStringLiteral("gelbooru");
        plain.fileUrl = QStringLiteral("https://img4.gelbooru.com/x.jpg");
        QVERIFY(!RemotePosts::needsWebDownload(plain));
        // Unknown sites fall back to the file host (old behavior).
        Post stray;
        stray.isRemote = true;
        stray.siteName = QStringLiteral("Nope");
        stray.fileUrl = QStringLiteral("https://booru.allthefallen.moe/z.jpg");
        QVERIFY(RemotePosts::needsWebDownload(stray));
        Post outer;
        outer.isRemote = true;
        outer.siteName = QStringLiteral("Nope");
        outer.fileUrl = QStringLiteral("https://example.com/z.jpg");
        QVERIFY(!RemotePosts::needsWebDownload(outer));
        RemoteSource::setConfiguredBoorus(RemoteSource::presetBoorus());
    }

    void engineKeysRoundTrip() {        for (BooruEngine e : {BooruEngine::Danbooru, BooruEngine::Gelbooru,
                              BooruEngine::Moebooru, BooruEngine::Szurubooru,
                              BooruEngine::Shimmie}) {
            QCOMPARE(RemotePosts::engineForKey(RemotePosts::engineKey(e)), e);
            QVERIFY(!RemotePosts::engineLabel(e).isEmpty());
        }
        QCOMPARE(RemotePosts::engineForKey(QStringLiteral("bogus")),
                 BooruEngine::Danbooru);
    }

    void engineForSiteKeyFollowsRegistry() {
        QCOMPARE(RemotePosts::engineForSiteKey(QStringLiteral("danbooru")),
                 BooruEngine::Danbooru);
        QCOMPARE(RemotePosts::engineForSiteKey(QStringLiteral("nope")),
                 BooruEngine::Danbooru); // unknown fallback
        BooruSite custom;
        custom.key = QStringLiteral("visuabusters");
        custom.label = QStringLiteral("Visuabusters");
        custom.baseUrl = QStringLiteral("https://www.visuabusters.com/booru");
        custom.engine = BooruEngine::Szurubooru;
        RemoteSource::setConfiguredBoorus({custom});
        QCOMPARE(RemotePosts::engineForSiteKey(QStringLiteral("visuabusters")),
                 BooruEngine::Szurubooru);
        // …and a custom instance refreshes through the family endpoints.
        Post p;
        p.isRemote = true;
        p.siteName = QStringLiteral("Visuabusters");
        p.id = 7;
        const RefreshTarget t = resolveRefreshTarget(p);
        QVERIFY(t.valid());
        QCOMPARE(t.siteKey, QStringLiteral("visuabusters"));
        QCOMPARE(t.baseUrl, QStringLiteral("https://www.visuabusters.com/booru"));
        RemoteSource::setConfiguredBoorus(RemoteSource::presetBoorus());
    }

    void uniqueKeys() {
        QCOMPARE(RemotePosts::uniqueBooruKey(QStringLiteral("My Booru"), {}),
                 QStringLiteral("mybooru"));
        QCOMPARE(RemotePosts::uniqueBooruKey(QStringLiteral("My Booru!"),
                                             QSet<QString>{QStringLiteral("mybooru")}),
                 QStringLiteral("mybooru2"));
        QCOMPARE(RemotePosts::uniqueBooruKey(QStringLiteral("!!!"), {}),
                 QStringLiteral("booru"));
    }

    void ratingTagsTranslate() {
        const QStringList tags = {QStringLiteral("1girl"),
                                  QStringLiteral("rating:safe")};
        QCOMPARE(RemotePosts::translateSearchTags(tags, BooruEngine::Danbooru),
                 tags);
        QCOMPARE(RemotePosts::translateSearchTags(tags, BooruEngine::Moebooru),
                 tags);
        QCOMPARE(RemotePosts::translateSearchTags(tags, BooruEngine::Shimmie),
                 tags);
        // Gelbooru has no rating:safe: narrow to general.
        QCOMPARE(RemotePosts::translateSearchTags(tags, BooruEngine::Gelbooru),
                 QStringList({QStringLiteral("1girl"),
                              QStringLiteral("rating:general")}));
        QCOMPARE(RemotePosts::translateSearchTags(tags, BooruEngine::Szurubooru),
                 QStringList({QStringLiteral("1girl"),
                              QStringLiteral("safety:safe")}));
        QCOMPARE(RemotePosts::translateSearchTags(
                     {QStringLiteral("rating:explicit")}, BooruEngine::Szurubooru),
                 QStringList({QStringLiteral("safety:unsafe")}));
        QCOMPARE(RemotePosts::translateSearchTags(
                     {QStringLiteral("rating:general")}, BooruEngine::Szurubooru),
                 QStringList({QStringLiteral("safety:safe")}));
        // Anything else passes through untouched.
        const QStringList other = {QStringLiteral("rating:explicit")};
        QCOMPARE(RemotePosts::translateSearchTags(other, BooruEngine::Danbooru),
                 other);
        QCOMPARE(RemotePosts::translateSearchTags(other, BooruEngine::Gelbooru),
                 other);
    }

    void ratingShorthandsTranslate() {
        // rating:g/s/q/e expand before the per-engine rules, so a
        // multibooru rating:g answers on every engine, not just the
        // ones that tolerate the shorthand.
        for (BooruEngine e : {BooruEngine::Danbooru, BooruEngine::Gelbooru,
                              BooruEngine::Moebooru, BooruEngine::Shimmie,
                              BooruEngine::Szurubooru,
                              BooruEngine::Safebooru}) {
            QCOMPARE(RemotePosts::translateSearchTags(
                         {QStringLiteral("1girl"),
                          QStringLiteral("rating:q")},
                         e),
                     QStringList({QStringLiteral("1girl"),
                                  QStringLiteral("rating:questionable")}));
        }
        QCOMPARE(RemotePosts::translateSearchTags(
                     {QStringLiteral("rating:g")}, BooruEngine::Danbooru),
                 QStringList({QStringLiteral("rating:general")}));
        QCOMPARE(RemotePosts::translateSearchTags(
                     {QStringLiteral("rating:G")}, BooruEngine::Gelbooru),
                 QStringList({QStringLiteral("rating:general")}));
        QCOMPARE(RemotePosts::translateSearchTags(
                     {QStringLiteral("rating:g")}, BooruEngine::Szurubooru),
                 QStringList({QStringLiteral("safety:safe")}));
        QCOMPARE(RemotePosts::translateSearchTags(
                     {QStringLiteral("rating:s")}, BooruEngine::Szurubooru),
                 QStringList({QStringLiteral("safety:safe")}));
        QCOMPARE(RemotePosts::translateSearchTags(
                     {QStringLiteral("rating:e")}, BooruEngine::Szurubooru),
                 QStringList({QStringLiteral("safety:unsafe")}));
        QCOMPARE(RemotePosts::translateSearchTags(
                     {QStringLiteral("rating:e")}, BooruEngine::Gelbooru),
                 QStringList({QStringLiteral("rating:explicit")}));
        // Non-shorthands and negations pass through untouched.
        QCOMPARE(RemotePosts::translateSearchTags(
                     {QStringLiteral("-rating:g")}, BooruEngine::Szurubooru),
                 QStringList({QStringLiteral("-rating:g")}));
        QCOMPARE(RemotePosts::translateSearchTags(
                     {QStringLiteral("rating:gg")}, BooruEngine::Szurubooru),
                 QStringList({QStringLiteral("rating:gg")}));
    }

    void sortTagsTranslate() {
        // Danbooru order:* (help:cheatsheet).
        QCOMPARE(RemotePosts::sortSearchTag(QStringLiteral("id-asc"),
                                            BooruEngine::Danbooru),
                 QStringLiteral("order:id_asc"));
        QCOMPARE(RemotePosts::sortSearchTag(QStringLiteral("score-desc"),
                                            BooruEngine::Danbooru),
                 QStringLiteral("order:score"));
        QCOMPARE(RemotePosts::sortSearchTag(QStringLiteral("score-asc"),
                                            BooruEngine::Danbooru),
                 QStringLiteral("order:score_asc"));
        QCOMPARE(RemotePosts::sortSearchTag(QStringLiteral("random"),
                                            BooruEngine::Danbooru),
                 QStringLiteral("order:random"));
        // Gelbooru family sort:*:* (help topic=cheatsheet).
        for (BooruEngine e : {BooruEngine::Gelbooru, BooruEngine::Safebooru}) {
            QCOMPARE(RemotePosts::sortSearchTag(QStringLiteral("id-asc"), e),
                     QStringLiteral("sort:id:asc"));
            QCOMPARE(RemotePosts::sortSearchTag(QStringLiteral("score-desc"), e),
                     QStringLiteral("sort:score:desc"));
            QCOMPARE(RemotePosts::sortSearchTag(QStringLiteral("score-asc"), e),
                     QStringLiteral("sort:score:asc"));
            // Random unverified there: stays client-side.
            QVERIFY(RemotePosts::sortSearchTag(QStringLiteral("random"), e)
                        .isEmpty());
        }
        // Moebooru order:id (konachan help/cheatsheet); the rest unverified.
        QCOMPARE(RemotePosts::sortSearchTag(QStringLiteral("id-asc"),
                                            BooruEngine::Moebooru),
                 QStringLiteral("order:id"));
        QVERIFY(RemotePosts::sortSearchTag(QStringLiteral("score-desc"),
                                           BooruEngine::Moebooru)
                    .isEmpty());
        // Szurubooru/Shimmie sort styles unverified: client-side.
        QVERIFY(RemotePosts::sortSearchTag(QStringLiteral("score-desc"),
                                           BooruEngine::Szurubooru)
                    .isEmpty());
        QVERIFY(RemotePosts::sortSearchTag(QStringLiteral("score-desc"),
                                           BooruEngine::Shimmie)
                    .isEmpty());
        // "added" and the id-desc default need no tag anywhere.
        for (BooruEngine e : {BooruEngine::Danbooru, BooruEngine::Gelbooru,
                              BooruEngine::Moebooru, BooruEngine::Szurubooru,
                              BooruEngine::Shimmie, BooruEngine::Safebooru}) {
            QVERIFY(
                RemotePosts::sortSearchTag(QStringLiteral("added"), e).isEmpty());
            QVERIFY(RemotePosts::sortSearchTag(QStringLiteral("id-desc"), e)
                        .isEmpty());
            QVERIFY(RemotePosts::sortSearchTag(QStringLiteral("bogus"), e)
                        .isEmpty());
        }
    }

    void transientErrors() {
        // Retryable: rate limits, timeouts, server errors, dead network.
        QVERIFY(RemotePosts::isTransientSearchError(
            429, QStringLiteral("HTTP 429 (Too Many Requests)")));
        QVERIFY(RemotePosts::isTransientSearchError(
            503, QStringLiteral("HTTP 503 (Service Unavailable)")));
        QVERIFY(RemotePosts::isTransientSearchError(
            408, QStringLiteral("HTTP 408 (Request Timeout)")));
        QVERIFY(RemotePosts::isTransientSearchError(
            0, QStringLiteral("Network error: Connection timed out")));
        // Never retry: auth, walls, bad filters, silence, cancels.
        QVERIFY(!RemotePosts::isTransientSearchError(
            401, QStringLiteral("HTTP 401 (Unauthorized)")));
        QVERIFY(!RemotePosts::isTransientSearchError(
            403, QStringLiteral("HTTP 403 — challenge")));
        QVERIFY(!RemotePosts::isTransientSearchError(
            400, QStringLiteral("HTTP 400 (Bad Request)")));
        QVERIFY(!RemotePosts::isTransientSearchError(200, QString()));
        QVERIFY(!RemotePosts::isTransientSearchError(
            0, QStringLiteral("cancelled")));
    }

    void danbooruMapping() {
        const Post p = RemotePosts::fromDanbooru(
            danbooruObj(), QStringLiteral("https://danbooru.donmai.us"));
        QCOMPARE(p.id, 100LL);
        QCOMPARE(p.score, 8);
        QCOMPARE(p.rating, QStringLiteral("general"));
        QCOMPARE(p.tagsArtist, QStringLiteral("an_artist"));
        QCOMPARE(p.tagsGeneral, QStringLiteral("1girl sky"));
        QCOMPARE(p.fileUrl, QStringLiteral("https://cdn.donmai.us/a/b.jpg"));
        QCOMPARE(p.sampleUrl, QStringLiteral("https://cdn.donmai.us/a/b_large.jpg"));
        QCOMPARE(p.previewUrl, QStringLiteral("https://cdn.donmai.us/a/b_prev.jpg"));
        QCOMPARE(p.ext, QStringLiteral("jpg"));
        QCOMPARE(p.postUrl, QStringLiteral("https://danbooru.donmai.us/posts/100"));
        QVERIFY(p.isRemote && p.hasMeta);
        QCOMPARE(p.siteName, QStringLiteral("Danbooru"));
        QCOMPARE(p.siteShort, QStringLiteral("DAN"));
        QCOMPARE(p.category, QStringLiteral("Danbooru"));
        QVERIFY(!p.rawJson.isEmpty());
        // finishRemote upgrades ratings from tags, like the local scanner.
        QJsonObject o = danbooruObj();
        o[QStringLiteral("tag_string_general")] = QStringLiteral("blowjob");
        const Post q = RemotePosts::fromDanbooru(
            o, QStringLiteral("https://danbooru.donmai.us"));
        QCOMPARE(q.rating, QStringLiteral("explicit"));
        QVERIFY(q.ratingInferred);
    }

    void danbooruFileFallbacks() {
        QJsonObject o = danbooruObj();
        o.remove(QStringLiteral("file_url")); // login-walled listing
        const Post p = RemotePosts::fromDanbooru(
            o, QStringLiteral("https://danbooru.donmai.us"));
        QCOMPARE(p.fileUrl, QStringLiteral("https://cdn.donmai.us/a/b_large.jpg"));
        QJsonObject bare;
        bare[QStringLiteral("file_ext")] = QStringLiteral("png");
        const Post q = RemotePosts::fromDanbooru(
            bare, QStringLiteral("https://danbooru.donmai.us"));
        QCOMPARE(q.ext, QStringLiteral("png"));
    }

    void gelbooruMapping() {
        QJsonObject o;
        o[QStringLiteral("id")] = 5;
        o[QStringLiteral("score")] = 3;
        o[QStringLiteral("rating")] = QStringLiteral("general");
        o[QStringLiteral("tags")] = QStringLiteral("1girl sky");
        o[QStringLiteral("width")] = 640;
        o[QStringLiteral("height")] = 480;
        o[QStringLiteral("file_url")] = QStringLiteral("https://img.gelbooru.com/a.jpg");
        o[QStringLiteral("sample_url")] = QStringLiteral("https://img.gelbooru.com/a_s.jpg");
        o[QStringLiteral("preview_url")] = QStringLiteral("https://img.gelbooru.com/a_p.jpg");
        const Post p = RemotePosts::fromGelbooru(o, QStringLiteral("https://gelbooru.com"));
        QCOMPARE(p.siteName, QStringLiteral("Gelbooru"));
        QCOMPARE(p.siteShort, QStringLiteral("GEL"));
        QCOMPARE(p.untyped, QStringLiteral("1girl sky"));
        QCOMPARE(p.postUrl,
                 QStringLiteral("https://gelbooru.com/index.php?page=post&s=view&id=5"));
    }

    void gelbooruRelativeUrlsResolve() {
        // Safebooru-style dapi answers carry relative file URLs.
        QJsonObject o;
        o[QStringLiteral("id")] = 6;
        o[QStringLiteral("score")] = 0;
        o[QStringLiteral("rating")] = QStringLiteral("s");
        o[QStringLiteral("tags")] = QStringLiteral("solo");
        o[QStringLiteral("file_url")] = QStringLiteral("11/ab.gif");
        const Post p = RemotePosts::fromGelbooru(o, QStringLiteral("https://safebooru.org"));
        QCOMPARE(p.siteName, QStringLiteral("Safebooru"));
        QCOMPARE(p.fileUrl, QStringLiteral("https://safebooru.org/images/11/ab.gif"));
        QCOMPARE(p.postUrl,
                 QStringLiteral("https://safebooru.org/index.php?page=post&s=view&id=6"));
    }

    void moebooruMapping() {
        QJsonObject o;
        o[QStringLiteral("id")] = 9;
        o[QStringLiteral("score")] = 2;
        o[QStringLiteral("rating")] = QStringLiteral("q");
        o[QStringLiteral("tags")] = QStringLiteral("twintails");
        o[QStringLiteral("created_at")] = 1700000000.0;
        o[QStringLiteral("file_url")] = QStringLiteral("https://files.yande.re/f.png");
        const Post y = RemotePosts::fromMoebooru(o, QStringLiteral("https://yande.re"));
        QCOMPARE(y.siteName, QStringLiteral("Yandere"));
        QCOMPARE(y.postUrl, QStringLiteral("https://yande.re/post/show/9"));
        QVERIFY(!y.date.isEmpty()); // epoch double -> ISO date
        const Post k = RemotePosts::fromMoebooru(o, QStringLiteral("https://konachan.com"));
        QCOMPARE(k.siteName, QStringLiteral("Konachan"));
        QCOMPARE(k.postUrl, QStringLiteral("https://konachan.com/post/show/9"));
    }

    void szuruMapping() {
        QJsonObject o;
        o[QStringLiteral("id")] = 12;
        o[QStringLiteral("score")] = 4;
        o[QStringLiteral("safety")] = QStringLiteral("unsafe");
        QJsonArray tags;
        auto tag = [](const char *name, const char *cat) {
            QJsonObject t;
            t[QStringLiteral("names")] = QJsonArray{QString::fromLatin1(name)};
            t[QStringLiteral("category")] = QString::fromLatin1(cat);
            return t;
        };
        tags.append(tag("an_artist", "artist"));
        tags.append(tag("some_series", "series")); // series -> copyright
        tags.append(tag("plain", "default"));
        o[QStringLiteral("tags")] = tags;
        o[QStringLiteral("canvasWidth")] = 1000;
        o[QStringLiteral("canvasHeight")] = 500;
        o[QStringLiteral("creationTime")] = QStringLiteral("2024-01-02T00:00:00Z");
        o[QStringLiteral("contentUrl")] = QStringLiteral("/data/a.jpg");
        const Post p = RemotePosts::fromSzuru(o, QStringLiteral("https://szu.example"));
        QCOMPARE(p.rating, QStringLiteral("explicit"));
        QCOMPARE(p.tagsArtist, QStringLiteral("an_artist"));
        QCOMPARE(p.tagsCopyright, QStringLiteral("some_series"));
        QCOMPARE(p.tagsGeneral, QStringLiteral("plain"));
        QCOMPARE(p.fileUrl, QStringLiteral("https://szu.example/data/a.jpg"));
        QCOMPARE(p.postUrl, QStringLiteral("https://szu.example/post/12"));
        QCOMPARE(p.siteShort, QStringLiteral("SZU"));
    }

    void szuruSafetyLevels() {
        for (auto [safety, rating] : {std::pair<const char *, const char *>{"safe", "general"},
                                      {"sketchy", "questionable"},
                                      {"questionable", "questionable"},
                                      {"sensitive", "sensitive"}}) {
            QJsonObject o;
            o[QStringLiteral("id")] = 1;
            o[QStringLiteral("safety")] = QString::fromLatin1(safety);
            o[QStringLiteral("tags")] = QJsonArray{};
            const Post p = RemotePosts::fromSzuru(o, QStringLiteral("https://x"));
            QCOMPARE(p.rating, QString(rating));
        }
    }

    void shimmieXmlParses() {
        BooruSite eff;
        eff.key = QStringLiteral("shimmie");
        eff.label = QStringLiteral("Shimmie");
        eff.shortName = QStringLiteral("SHI");
        eff.baseUrl = QStringLiteral("https://sh.example");
        eff.engine = BooruEngine::Shimmie;
        const QByteArray xml =
            "<posts count=\"1\" offset=\"0\">"
            "<post id=\"7\" md5=\"abc123\" file_name=\"x.png\" "
            "file_url=\"https://sh.example/_images/abc123.png\" height=\"100\" width=\"200\" "
            "preview_url=\"/_thumbs/abc123.jpg\" rating=\"?\" date=\"2024-01-01 00:00:00\" "
            "tags=\"1girl long_hair\" source=\"https://src.example\" score=\"3\" />"
            "</posts>";
        const SearchResult r = RemotePosts::parseShimmieXml(eff, xml);
        QVERIFY(r.ok());
        QCOMPARE(r.posts.size(), 1);
        const Post &p = r.posts.first();
        QCOMPARE(p.id, 7LL);
        QCOMPARE(p.rating, QStringLiteral("general")); // "?" -> general
        QCOMPARE(p.tagsGeneral, QStringLiteral("1girl long_hair"));
        QCOMPARE(p.fileUrl, QStringLiteral("https://sh.example/_images/abc123.png"));
        QCOMPARE(p.previewUrl, QStringLiteral("https://sh.example/_thumbs/abc123.jpg"));
        QCOMPARE(p.postUrl, QStringLiteral("https://sh.example/post/view/7"));
        QCOMPARE(p.siteShort, QStringLiteral("SHI"));
    }

    void shimmieXmlGarbageFails() {
        BooruSite eff;
        eff.key = QStringLiteral("shimmie");
        eff.label = QStringLiteral("Shimmie");
        eff.baseUrl = QStringLiteral("https://sh.example");
        eff.engine = BooruEngine::Shimmie;
        const SearchResult r =
            RemotePosts::parseShimmieXml(eff, QByteArray("this is not xml <"));
        QVERIFY(!r.ok());
        QVERIFY(r.posts.isEmpty());
    }

    void postPageUrls() {
        QCOMPARE(RemotePosts::postPageUrl(QStringLiteral("danbooru"),
                                          QStringLiteral("https://d"), 1),
                 QStringLiteral("https://d/posts/1"));
        QCOMPARE(RemotePosts::postPageUrl(QStringLiteral("yandere"),
                                          QStringLiteral("https://y"), 2),
                 QStringLiteral("https://y/post/show/2"));
        QCOMPARE(RemotePosts::postPageUrl(QStringLiteral("konachan"),
                                          QStringLiteral("https://k"), 3),
                 QStringLiteral("https://k/post/show/3"));
        QCOMPARE(RemotePosts::postPageUrl(QStringLiteral("szurubooru"),
                                          QStringLiteral("https://s"), 4),
                 QStringLiteral("https://s/post/4"));
        QCOMPARE(RemotePosts::postPageUrl(QStringLiteral("shimmie"),
                                          QStringLiteral("https://h"), 5),
                 QStringLiteral("https://h/post/view/5"));
        QCOMPARE(RemotePosts::postPageUrl(QStringLiteral("gelbooru"),
                                          QStringLiteral("https://g"), 6),
                 QStringLiteral("https://g/index.php?page=post&s=view&id=6"));
    }

    void downloadFileNames() {
        QJsonObject md5;
        md5[QStringLiteral("md5")] = QStringLiteral("abcdef0123456789abcdef0123456789");
        md5[QStringLiteral("file_ext")] = QStringLiteral("png");
        QCOMPARE(RemotePosts::downloadFileName(QStringLiteral("Gelbooru"), md5),
                 QStringLiteral("Gelbooru_abcdef0123456789abcdef0123456789.png"));
        QJsonObject szu; // szurubooru checksum field + ext from URL
        szu[QStringLiteral("checksumMD5")] = QStringLiteral("0123456789abcdef0123456789abcdef");
        szu[QStringLiteral("file_url")] = QStringLiteral("https://x/f.webm");
        QCOMPARE(RemotePosts::downloadFileName(QStringLiteral("Szurubooru"), szu),
                 QStringLiteral("Szurubooru_0123456789abcdef0123456789abcdef.webm"));
        QJsonObject byId; // no hash anywhere: id fallback, jpg default
        byId[QStringLiteral("id")] = 77;
        QCOMPARE(RemotePosts::downloadFileName(QStringLiteral("Danbooru"), byId),
                 QStringLiteral("Danbooru_77.jpg"));
        // Labels are filename-sanitized; empty labels fall back to Booru.
        QCOMPARE(RemotePosts::downloadFileName(QStringLiteral("Rule 34!"), byId),
                 QStringLiteral("Rule34_77.jpg"));
        QCOMPARE(RemotePosts::downloadFileName(QString(), byId),
                 QStringLiteral("Booru_77.jpg"));
    }

    void disabledBucketsFoldToGeneral() {
        QJsonObject o = danbooruObj();
        o[QStringLiteral("tag_string_character")] = QStringLiteral("some_character");
        o[QStringLiteral("tag_string_copyright")] = QStringLiteral("some_series");
        const Post plain = RemotePosts::fromDanbooru(
            o, QStringLiteral("https://danbooru.donmai.us"));
        QVERIFY(plain.tagsCharacter.contains(QStringLiteral("some_character")));
        RemotePosts::setDisabledBuckets(
            {QStringLiteral("danbooru:character"),
             QStringLiteral("danbooru:copyright")});
        const Post folded = RemotePosts::fromDanbooru(
            o, QStringLiteral("https://danbooru.donmai.us"));
        QVERIFY(folded.tagsCharacter.isEmpty());
        QVERIFY(folded.tagsCopyright.isEmpty());
        QVERIFY(folded.tagsGeneral.contains(QStringLiteral("some_character")));
        QVERIFY(folded.tagsGeneral.contains(QStringLiteral("some_series")));
        QVERIFY(folded.tagsArtist.contains(QStringLiteral("an_artist")));
        RemotePosts::setDisabledBuckets({});
    }

    void skipAutoClassifyFlag() {
        BooruSite meme;
        meme.key = QStringLiteral("memebooru");
        meme.label = QStringLiteral("Memebooru");
        meme.baseUrl = QStringLiteral("https://meme.example");
        meme.engine = BooruEngine::Gelbooru;
        meme.skipAutoClassify = true;
        RemoteSource::setConfiguredBoorus({meme});
        Post p;
        p.isRemote = true;
        p.siteName = QStringLiteral("Memebooru");
        p.siteKey = QStringLiteral("memebooru");
        QVERIFY(RemotePosts::skipAutoClassify(p));
        Post q;
        q.isRemote = true;
        q.siteName = QStringLiteral("Gelbooru");
        q.siteKey = QStringLiteral("gelbooru");
        QVERIFY(!RemotePosts::skipAutoClassify(q));
        Post local;
        local.absFile = QStringLiteral("/x.jpg");
        QVERIFY(!RemotePosts::skipAutoClassify(local));
        RemoteSource::setConfiguredBoorus({});
    }

    void redactSecrets() {
        QCOMPARE(RemotePosts::redactSecrets(
                     QStringLiteral("https://gelbooru.com/index.php?page=dapi&"
                                    "api_key=SECRET123&user_id=7")),
                 QStringLiteral("https://gelbooru.com/index.php?page=dapi&"
                                "api_key=<redacted>&user_id=7"));
        QCOMPARE(RemotePosts::redactSecrets(
                     QStringLiteral("https://x/api/posts/?token=abc&query=safe")),
                 QStringLiteral("https://x/api/posts/?token=<redacted>&query=safe"));
        QCOMPARE(RemotePosts::redactSecrets(
                     QStringLiteral("https://danbooru.donmai.us/posts.json?tags=a")),
                 QStringLiteral("https://danbooru.donmai.us/posts.json?tags=a"));
        QCOMPARE(RemotePosts::redactSecrets(QStringLiteral("https://x/?API_KEY=z")),
                 QStringLiteral("https://x/?API_KEY=<redacted>"));
    }

    void labelHelpers() {
        QCOMPARE(RemotePosts::shortFor(QStringLiteral("Danbooru")), QStringLiteral("DAN"));
        QCOMPARE(RemotePosts::shortFor(QStringLiteral("Szurubooru")), QStringLiteral("SZU"));
        QCOMPARE(RemotePosts::shortFor(QStringLiteral("Rule34")), QStringLiteral("RUL"));
        QCOMPARE(RemotePosts::keyForLabel(QStringLiteral("Konachan")),
                 QStringLiteral("konachan"));
        QCOMPARE(RemotePosts::keyForLabel(QStringLiteral("Shimmie")),
                 QStringLiteral("shimmie"));
        QVERIFY(RemotePosts::keyForLabel(QStringLiteral("Rule34")).isEmpty());
    }

    void urlHelpers() {
        Post p;
        p.previewUrl = QStringLiteral("https://x/prev.jpg");
        p.sampleUrl = QStringLiteral("https://x/sample.jpg");
        p.fileUrl = QStringLiteral("https://x/full.jpg");
        QCOMPARE(RemotePosts::thumbUrlFor(p), QStringLiteral("https://x/prev.jpg"));
        QCOMPARE(RemotePosts::viewerUrlFor(p), QStringLiteral("https://x/sample.jpg"));
        p.previewUrl.clear();
        QCOMPARE(RemotePosts::thumbUrlFor(p), QStringLiteral("https://x/sample.jpg"));
        p.sampleUrl.clear();
        QCOMPARE(RemotePosts::thumbUrlFor(p), QStringLiteral("https://x/full.jpg"));
        QCOMPARE(RemotePosts::viewerUrlFor(p), QStringLiteral("https://x/full.jpg"));
        QVERIFY(RemotePosts::isImageUrl(QStringLiteral("https://x/a.png")));
        QVERIFY(RemotePosts::isImageUrl(QStringLiteral("https://x/a.webp")));
        QVERIFY(!RemotePosts::isImageUrl(QStringLiteral("https://x/a.mp4")));
        QVERIFY(!RemotePosts::isImageUrl(QString()));
    }

    void parseSingleShapes() {
        bool ok = false;
        const QJsonObject d = PostRefresher::parseSingle(
            BooruEngine::Danbooru, QByteArray("{\"id\":42}"), ok);
        QVERIFY(ok);
        QCOMPARE(d[QStringLiteral("id")].toDouble(), 42.0);
        const QJsonObject y = PostRefresher::parseSingle(
            BooruEngine::Moebooru, QByteArray("[{\"id\":1},{\"id\":2}]"), ok);
        QVERIFY(ok);
        QCOMPARE(y[QStringLiteral("id")].toDouble(), 1.0);
        const QJsonObject g = PostRefresher::parseSingle(
            BooruEngine::Gelbooru, QByteArray("{\"post\":[{\"id\":3}]}"), ok);
        QVERIFY(ok);
        QCOMPARE(g[QStringLiteral("id")].toDouble(), 3.0);
        const QJsonObject gb = PostRefresher::parseSingle(
            BooruEngine::Gelbooru, QByteArray("[{\"id\":4}]"), ok);
        QVERIFY(ok);
        QCOMPARE(gb[QStringLiteral("id")].toDouble(), 4.0);
        const QJsonObject s = PostRefresher::parseSingle(
            BooruEngine::Shimmie,
            QByteArray("<posts><post id=\"9\" tags=\"solo\"/></posts>"), ok);
        QVERIFY(ok);
        QCOMPARE(s[QStringLiteral("id")].toDouble(), 9.0);
        QCOMPARE(s[QStringLiteral("tags")].toString(), QStringLiteral("solo"));
        PostRefresher::parseSingle(BooruEngine::Danbooru, QByteArray("{{{"),
                                   ok);
        QVERIFY(!ok);
    }

    void refreshTargets() {
        // Registry-backed: configure the entries this test resolves.
        BooruSite dan;
        dan.key = QStringLiteral("danbooru");
        dan.label = QStringLiteral("Danbooru");
        dan.shortName = QStringLiteral("DAN");
        dan.baseUrl = QStringLiteral("https://danbooru.donmai.us");
        dan.engine = BooruEngine::Danbooru;
        RemoteSource::setConfiguredBoorus({dan});
        Post remote;
        remote.isRemote = true;
        remote.siteName = QStringLiteral("Danbooru");
        remote.id = 5;
        RefreshTarget t = resolveRefreshTarget(remote);
        QVERIFY(t.valid());
        QCOMPARE(t.siteKey, QStringLiteral("danbooru"));
        QCOMPARE(t.baseUrl, QStringLiteral("https://danbooru.donmai.us"));
        QCOMPARE(t.id, 5LL);

        Post szu;
        szu.isRemote = true;
        szu.siteName = QStringLiteral("Szurubooru");
        szu.id = 6;
        QVERIFY(!resolveRefreshTarget(szu).valid()); // no instance URL configured
        BooruSite szuCfg;
        szuCfg.key = QStringLiteral("szurubooru");
        szuCfg.label = QStringLiteral("Szurubooru");
        szuCfg.shortName = QStringLiteral("SZU");
        szuCfg.baseUrl = QStringLiteral("https://szu.example");
        szuCfg.engine = BooruEngine::Szurubooru;
        szuCfg.user = QStringLiteral("u");
        RemoteSource::setConfiguredBoorus({dan, szuCfg});
        RefreshTarget t2 = resolveRefreshTarget(szu);
        QVERIFY(t2.valid());
        QCOMPARE(t2.baseUrl, QStringLiteral("https://szu.example"));
        QCOMPARE(t2.creds.user, QStringLiteral("u"));
        RemoteSource::setConfiguredBoorus(RemoteSource::presetBoorus());

        Post unknown;
        unknown.isRemote = true;
        unknown.siteName = QStringLiteral("Rule34");
        unknown.id = 1;
        QVERIFY(!resolveRefreshTarget(unknown).valid());

        // Local LOSN sidecars resolve via post URL host.
        RemoteSource::setConfiguredBoorus({dan});
        Post local;
        local.postUrl = QStringLiteral("https://danbooru.donmai.us/posts/8");
        local.id = 8;
        RefreshTarget t3 = resolveRefreshTarget(local);
        QVERIFY(t3.valid());
        QCOMPARE(t3.siteKey, QStringLiteral("danbooru"));

        Post gallerydl; // no source link: not refreshable
        gallerydl.id = 8;
        QVERIFY(!resolveRefreshTarget(gallerydl).valid());
        Post noid;
        noid.postUrl = QStringLiteral("https://danbooru.donmai.us/posts/8");
        QVERIFY(!resolveRefreshTarget(noid).valid());
        RemoteSource::setConfiguredBoorus(RemoteSource::presetBoorus());
    }

    void cookiesNormalize() {
        using RemotePosts::normalizeCookies;
        // Plain lines pass through (sorted, later wins).
        QCOMPARE(normalizeCookies({QStringLiteral("b=2"), QStringLiteral("a=1")}),
                 QStringList({QStringLiteral("a=1"), QStringLiteral("b=2")}));
        QCOMPARE(normalizeCookies({QStringLiteral("a=1"), QStringLiteral("a=2")}),
                 QStringList({QStringLiteral("a=2")}));
        // Pasted header blobs split on ';'.
        QCOMPARE(normalizeCookies({QStringLiteral("a=1; b=2")}),
                 QStringList({QStringLiteral("a=1"), QStringLiteral("b=2")}));
        // Set-Cookie attributes and garbage are dropped.
        QCOMPARE(normalizeCookies({QStringLiteral("cf_clearance=abc; Path=/; HttpOnly"),
                                   QStringLiteral("noequals"),
                                   QStringLiteral("=noname"),
                                   QStringLiteral("empty=")}),
                 QStringList({QStringLiteral("cf_clearance=abc")}));
        QVERIFY(normalizeCookies({}).isEmpty());
        QCOMPARE(RemotePosts::cookieHeaderValue(
                     {QStringLiteral("b=2"), QStringLiteral("a=1")}),
                 QStringLiteral("a=1; b=2"));
        QVERIFY(RemotePosts::cookieHeaderValue({}).isEmpty());
    }

    void cookiesApply() {
        BooruSite walled;
        walled.key = QStringLiteral("walled");
        walled.label = QStringLiteral("Walled");
        walled.baseUrl = QStringLiteral("https://booru.example");
        walled.engine = BooruEngine::Danbooru;
        walled.cookies = {QStringLiteral("cf_clearance=abc"),
                          QStringLiteral("sess=xyz")};
        RemoteSource::setConfiguredBoorus({walled});
        // Exact host gets the header.
        QNetworkRequest api(QUrl(QStringLiteral("https://booru.example/posts.json")));
        RemoteSource::applyCookies(api);
        QCOMPARE(api.rawHeader("Cookie"),
                 QByteArray("cf_clearance=abc; sess=xyz"));
        // Subdomain (media CDN next door) too.
        QNetworkRequest cdn(QUrl(QStringLiteral("https://cdn.booru.example/a.jpg")));
        RemoteSource::applyCookies(cdn);
        QCOMPARE(cdn.rawHeader("Cookie"),
                 QByteArray("cf_clearance=abc; sess=xyz"));
        // Unrelated hosts and cookieless setups stay clean.
        QNetworkRequest other(QUrl(QStringLiteral("https://other.example/x")));
        RemoteSource::applyCookies(other);
        QVERIFY(!other.hasRawHeader("Cookie"));
        QNetworkRequest bare(QUrl(QStringLiteral("https://booru.example/x")));
        RemoteSource::setConfiguredBoorus({});
        RemoteSource::applyCookies(bare);
        QVERIFY(!bare.hasRawHeader("Cookie"));
        RemoteSource::setConfiguredBoorus(RemoteSource::presetBoorus());
    }

    void siteQuirks() {
        // Gelbooru ships its restricted-content cookie.
        const RemotePosts::SiteQuirk *g =
            RemotePosts::quirkForHost(QStringLiteral("gelbooru.com"));
        QVERIFY(g);
        QVERIFY(g->defaultCookies.contains(
            QStringLiteral("fringeBenefits=yup")));
        QVERIFY(!g->powWall);
        // Case-insensitive lookup.
        QVERIFY(RemotePosts::quirkForHost(
            QStringLiteral("Gelbooru.COM")) == g);
        // rule34 API moves off the captcha-walled main host.
        QCOMPARE(RemotePosts::apiBaseUrl(
                     BooruEngine::Gelbooru,
                     QStringLiteral("https://rule34.xxx")),
                 QStringLiteral("https://api.rule34.xxx"));
        // Non-Gelbooru engines are unaffected by that override.
        QCOMPARE(RemotePosts::apiBaseUrl(
                     BooruEngine::Danbooru,
                     QStringLiteral("https://rule34.xxx")),
                 QStringLiteral("https://rule34.xxx"));
        // ATF is flagged as a periodic proof-of-work wall.
        const RemotePosts::SiteQuirk *atf = RemotePosts::quirkForHost(
            QStringLiteral("booru.allthefallen.moe"));
        QVERIFY(atf);
        QVERIFY(atf->powWall);
        QVERIFY(atf->defaultCookies.isEmpty());
        // Unknown hosts have no quirk and pass through.
        QVERIFY(!RemotePosts::quirkForHost(
            QStringLiteral("danbooru.donmai.us")));
        QCOMPARE(RemotePosts::apiBaseUrl(
                     BooruEngine::Gelbooru,
                     QStringLiteral("https://gelbooru.com")),
                 QStringLiteral("https://gelbooru.com"));
    }

    void proxySpec() {
        using RemotePosts::proxyFromSpec;
        // Empty/garbage means direct (same as never configured).
        QCOMPARE(proxyFromSpec(QString()).type(), QNetworkProxy().type());
        QCOMPARE(proxyFromSpec(QStringLiteral("not a url")).type(),
                 QNetworkProxy().type());
        QCOMPARE(proxyFromSpec(QStringLiteral("ftp://h:21")).type(),
                 QNetworkProxy().type());
        const QNetworkProxy http =
            proxyFromSpec(QStringLiteral("http://127.0.0.1:8080"));
        QCOMPARE(http.type(), QNetworkProxy::HttpProxy);
        QCOMPARE(http.hostName(), QStringLiteral("127.0.0.1"));
        QCOMPARE(http.port(), 8080);
        const QNetworkProxy socks = proxyFromSpec(
            QStringLiteral("socks5://u:p@proxy.example:1080"));
        QCOMPARE(socks.type(), QNetworkProxy::Socks5Proxy);
        QCOMPARE(socks.hostName(), QStringLiteral("proxy.example"));
        QCOMPARE(socks.port(), 1080);
        QCOMPARE(socks.user(), QStringLiteral("u"));
        QCOMPARE(socks.password(), QStringLiteral("p"));
        // socks5h parses to the same remote-DNS SOCKS5 type.
        const QNetworkProxy socksH =
            proxyFromSpec(QStringLiteral("socks5h://127.0.0.1:9050"));
        QCOMPARE(socksH.type(), QNetworkProxy::Socks5Proxy);
        QCOMPARE(socksH.hostName(), QStringLiteral("127.0.0.1"));
        QCOMPARE(socksH.port(), 9050);
    }

    void globalProxyFallback() {
        using RemotePosts::proxyFromSpec;
        RemotePosts::setGlobalProxy(QString());
        // No global: empty stays direct (default proxy).
        QCOMPARE(proxyFromSpec(QString()).type(),
                 QNetworkProxy().type());
        // Global set: empty specs ride it, explicit specs win.
        RemotePosts::setGlobalProxy(QStringLiteral("socks5h://127.0.0.1:9050"));
        const QNetworkProxy g = proxyFromSpec(QString());
        QCOMPARE(g.type(), QNetworkProxy::Socks5Proxy);
        QCOMPARE(g.hostName(), QStringLiteral("127.0.0.1"));
        QCOMPARE(g.port(), 9050);
        const QNetworkProxy site =
            proxyFromSpec(QStringLiteral("http://127.0.0.1:8080"));
        QCOMPARE(site.type(), QNetworkProxy::HttpProxy);
        QCOMPARE(site.port(), 8080);
        // "direct" opts one entry back out of a set global.
        // (QNetworkProxy() default-constructs to DefaultProxy,
        // which QNAM treats as direct.)
        QCOMPARE(proxyFromSpec(QStringLiteral("direct")).type(),
                 QNetworkProxy().type());
        QCOMPARE(proxyFromSpec(QStringLiteral("NONE")).type(),
                 QNetworkProxy().type());
        RemotePosts::setGlobalProxy(QString());
    }

    void proxyForUrl() {        BooruSite proxied;
        proxied.key = QStringLiteral("proxied");
        proxied.label = QStringLiteral("Proxied");
        proxied.baseUrl = QStringLiteral("https://booru.example");
        proxied.engine = BooruEngine::Danbooru;
        proxied.proxy = QStringLiteral("http://127.0.0.1:8080");
        BooruSite plain;
        plain.key = QStringLiteral("plain");
        plain.label = QStringLiteral("Plain");
        plain.baseUrl = QStringLiteral("https://other.example");
        plain.engine = BooruEngine::Danbooru;
        RemoteSource::setConfiguredBoorus({proxied, plain});
        QCOMPARE(RemotePosts::proxyForUrl(
                     QStringLiteral("https://booru.example/posts.json")),
                 QStringLiteral("http://127.0.0.1:8080"));
        // Subdomains (media CDNs) inherit the entry's proxy.
        QCOMPARE(RemotePosts::proxyForUrl(
                     QStringLiteral("https://cdn.booru.example/a.jpg")),
                 QStringLiteral("http://127.0.0.1:8080"));
        // No proxy configured: direct.
        QVERIFY(RemotePosts::proxyForUrl(
                    QStringLiteral("https://other.example/x")).isEmpty());
        QVERIFY(RemotePosts::proxyForUrl(
                    QStringLiteral("https://unknown.example/x")).isEmpty());
        RemoteSource::setConfiguredBoorus(RemoteSource::presetBoorus());
    }

    void harvestCookies() {
        using RemotePosts::harvestCookies;
        QList<QNetworkCookie> jar;
        QNetworkCookie cf(QStringLiteral("cf_clearance").toUtf8(), "abc123");
        cf.setDomain(QStringLiteral(".booru.example"));
        jar << cf;
        QNetworkCookie sess(QStringLiteral("sess").toUtf8(), "xyz");
        sess.setDomain(QStringLiteral("booru.example"));
        jar << sess;
        // Later arrivals win.
        QNetworkCookie sess2(QStringLiteral("sess").toUtf8(), "new");
        sess2.setDomain(QStringLiteral("booru.example"));
        jar << sess2;
        // Wrong host + garbage are dropped.
        QNetworkCookie other(QStringLiteral("x").toUtf8(), "1");
        other.setDomain(QStringLiteral("other.example"));
        jar << other;
        QNetworkCookie noname(QByteArray(), "v");
        noname.setDomain(QStringLiteral("booru.example"));
        jar << noname;
        QNetworkCookie noval(QStringLiteral("n").toUtf8(), QByteArray());
        noval.setDomain(QStringLiteral("booru.example"));
        jar << noval;
        const QStringList out =
            harvestCookies(jar, QStringLiteral("booru.example"));
        QVERIFY(out.contains(QStringLiteral("cf_clearance=abc123")));
        QVERIFY(out.contains(QStringLiteral("sess=new")));
        QVERIFY(!out.join(u';').contains(QStringLiteral("sess=xyz")));
        QVERIFY(!out.join(u';').contains(QStringLiteral("x=1")));
        // Subdomain cookies cover the exact host too.
        QNetworkCookie sub(QStringLiteral("s").toUtf8(), "1");
        sub.setDomain(QStringLiteral("cdn.booru.example"));
        QVERIFY(harvestCookies({sub}, QStringLiteral("booru.example"))
                    .contains(QStringLiteral("s=1")));
        // Host-only session cookies (empty domain) are kept.
        QNetworkCookie hostOnly(QStringLiteral("h").toUtf8(), "1");
        QVERIFY(harvestCookies({hostOnly}, QStringLiteral("booru.example"))
                    .contains(QStringLiteral("h=1")));
        QVERIFY(harvestCookies({}, QStringLiteral("booru.example")).isEmpty());
        QVERIFY(harvestCookies(jar, QString()).isEmpty());
    }
};

int main(int argc, char *argv[]) {
    // QCoreApplication (not appless): QSqlDatabase and friends
    // require an application instance. Still no display needed.
    QCoreApplication app(argc, argv);
    TestRemoteSource tc;
    return QTest::qExec(&tc, argc, argv);
}
#include "test_remotesource.moc"
