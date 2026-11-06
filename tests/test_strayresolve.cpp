#include "strayresolve.h"
#include "metaparser.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QtTest>

class TestStrayResolve : public QObject {
    Q_OBJECT

private slots:
    void booruPostUrls_data() {
        QTest::addColumn<QString>("url");
        QTest::addColumn<bool>("valid");
        QTest::addColumn<qint64>("id");
        QTest::addColumn<QString>("base");
        QTest::addColumn<int>("firstEngine");

        const int dan = int(BooruEngine::Danbooru);
        const int gel = int(BooruEngine::Gelbooru);
        const int moe = int(BooruEngine::Moebooru);
        const int szu = int(BooruEngine::Szurubooru);
        const int shi = int(BooruEngine::Shimmie);
        const int nuu = int(BooruEngine::Nuuru);
        const int der = int(BooruEngine::Derpibooru);

        QTest::newRow("danbooru")
            << "https://danbooru.donmai.us/posts/123?q=foo" << true
            << qint64(123) << "https://danbooru.donmai.us" << dan;
        QTest::newRow("e621-posts")
            << "https://e621.net/posts/456" << true << qint64(456)
            << "https://e621.net" << dan;
        QTest::newRow("gelbooru")
            << "https://gelbooru.com/index.php?page=post&s=view&id=77"
            << true << qint64(77) << "https://gelbooru.com" << gel;
        QTest::newRow("safebooru-subpath")
            << "https://example.org/booru/index.php?page=post&s=view&id=8"
            << true << qint64(8) << "https://example.org/booru" << gel;
        QTest::newRow("moebooru")
            << "https://yande.re/post/show/900" << true << qint64(900)
            << "https://yande.re" << moe;
        QTest::newRow("szurubooru")
            << "https://booru.example/post/42" << true << qint64(42)
            << "https://booru.example" << szu;
        QTest::newRow("shimmie-query")
            << "https://shim.example/index.php?q=post/view/13" << true
            << qint64(13) << "https://shim.example" << shi;
        QTest::newRow("post-view")
            << "https://booru.example/post/view/21" << true << qint64(21)
            << "https://booru.example" << nuu;
        QTest::newRow("derpibooru")
            << "https://derpibooru.org/images/3926792" << true << qint64(3926792)
            << "https://derpibooru.org" << der;
        QTest::newRow("port")
            << "https://booru.example:8443/posts/5" << true << qint64(5)
            << "https://booru.example:8443" << dan;
        QTest::newRow("direct image")
            << "https://cdn.example/files/cat.jpg" << false << qint64(0)
            << QString() << int(BooruEngine::Danbooru);
        QTest::newRow("not-http")
            << "file:///tmp/cat.jpg" << false << qint64(0) << QString()
            << int(BooruEngine::Danbooru);
    }

    void booruPostUrls() {
        QFETCH(QString, url);
        QFETCH(bool, valid);
        QFETCH(qint64, id);
        QFETCH(QString, base);
        QFETCH(int, firstEngine);
        const auto m = RemotePosts::parseBooruPostUrl(url);
        QCOMPARE(m.valid(), valid);
        if (!valid)
            return;
        QCOMPARE(m.id, id);
        QCOMPARE(m.baseUrl, base);
        QVERIFY(!m.engines.isEmpty());
        QCOMPARE(int(m.engines.first()), firstEngine);
    }

    void postViewProbesBothFamilies() {
        const auto m = RemotePosts::parseBooruPostUrl(
            "https://booru.example/post/view/21");
        QVERIFY(m.valid());
        QCOMPARE(int(m.engines.size()), 2);
        QCOMPARE(int(m.engines[0]), int(BooruEngine::Nuuru));
        QCOMPARE(int(m.engines[1]), int(BooruEngine::Shimmie));
    }

    void postsPatternIncludesTwibooru() {
        const auto m = RemotePosts::parseBooruPostUrl(
            "https://twibooru.org/posts/3889209");
        QVERIFY(m.valid());
        QVERIFY(m.engines.contains(BooruEngine::Twibooru));
    }

    void htmlPage() {
        const QByteArray html =
            "<html><head>\n"
            "<meta property=\"og:image\" content=\"https://img.example/posts/full/abc.jpg\" />\n"
            "<meta property=\"og:title\" content=\"Example post\" />\n"
            "<meta name=\"rating\" content=\"questionable\" />\n"
            "</head><body>\n"
            "<section data-id=\"321\" data-md5=\"0123456789abcdef0123456789abcdef\"\n"
            "         data-rating=\"questionable\" data-score=\"12\" data-width=\"800\"\n"
            "         data-height=\"600\" data-tags=\"ignored_when_bucketed\" />\n"
            "<li class=\"tag-type-artist\"><a href=\"index.php?page=post&amp;s=list&amp;tags=some_artist\">some_artist</a></li>\n"
            "<li class=\"tag-type-character\"><a href=\"index.php?page=post&amp;s=list&amp;tags=some_char\">some_char</a></li>\n"
            "<li class=\"tag-type-copyright\"><a href=\"index.php?page=post&amp;s=list&amp;tags=some_series\">some_series</a></li>\n"
            "<li class=\"tag-type-general\"><a href=\"index.php?page=post&amp;s=list&amp;tags=1girl\">1girl</a></li>\n"
            "<li class=\"tag-type-metadata\"><a href=\"index.php?page=post&amp;s=list&amp;tags=highres\">highres</a></li>\n"
            "</body></html>\n";
        const Post p = RemotePosts::parseBooruPageHtml(
            html, "https://booru.example/index.php?page=post&s=view&id=321",
            321);
        QCOMPARE(p.fileUrl, QStringLiteral("https://img.example/posts/full/abc.jpg"));
        QCOMPARE(p.id, qint64(321));
        QCOMPARE(p.md5, QStringLiteral("0123456789abcdef0123456789abcdef"));
        QCOMPARE(p.rating, QStringLiteral("questionable"));
        QCOMPARE(p.score, 12);
        QCOMPARE(p.width, 800);
        QCOMPARE(p.height, 600);
        QCOMPARE(p.tagsArtist, QStringLiteral("some_artist"));
        QCOMPARE(p.tagsCharacter, QStringLiteral("some_char"));
        QCOMPARE(p.tagsCopyright, QStringLiteral("some_series"));
        QCOMPARE(p.tagsGeneral, QStringLiteral("1girl"));
        QCOMPARE(p.tagsMeta, QStringLiteral("highres"));
        QCOMPARE(p.tags, QStringLiteral("some_artist some_char some_series 1girl highres"));
        QVERIFY(p.postUrl.startsWith(QStringLiteral("https://booru.example/")));
    }

    void htmlPageWithoutMedia() {
        const Post p = RemotePosts::parseBooruPageHtml(
            QByteArray("<html><body>login wall</body></html>"),
            "https://booru.example/post/1", 1);
        QVERIFY(p.fileUrl.isEmpty());
    }

    void htmlSocialPreviewIsNotAPost() {
        // A 404/challenge page still carries the site's og:image; without
        // booru post markup it must never become a download.
        const Post p = RemotePosts::parseBooruPageHtml(
            QByteArray("<html><head><meta property=\"og:image\" "
                       "content=\"https://site.example/banner.png\" />"
                       "</head><body>Not found</body></html>"),
            "https://booru.example/post/1", 1);
        QVERIFY(p.fileUrl.isEmpty());
        QVERIFY(p.tags.isEmpty());
    }

    void e621Shape() {
        QJsonObject file;
        file[QStringLiteral("width")] = 707;
        file[QStringLiteral("height")] = 1000;
        file[QStringLiteral("ext")] = QStringLiteral("jpg");
        file[QStringLiteral("md5")] =
            QStringLiteral("8a977dc4717a72ddc8e5dc3cfd1fbd48");
        file[QStringLiteral("url")] =
            QStringLiteral("https://static.example/data/8a/97/x.jpg");
        QJsonObject preview;
        preview[QStringLiteral("url")] =
            QStringLiteral("https://static.example/preview/x.jpg");
        QJsonObject tags;
        tags[QStringLiteral("general")] = QJsonArray{QStringLiteral("solo")};
        tags[QStringLiteral("artist")] = QJsonArray{QStringLiteral("painter")};
        tags[QStringLiteral("character")] =
            QJsonArray{QStringLiteral("zangoose")};
        tags[QStringLiteral("copyright")] = QJsonArray{QStringLiteral("pokemon")};
        tags[QStringLiteral("species")] =
            QJsonArray{QStringLiteral("pokemon_(species)")};
        tags[QStringLiteral("meta")] = QJsonArray{};
        QJsonObject score;
        score[QStringLiteral("total")] = 7;
        QJsonObject raw;
        raw[QStringLiteral("id")] = 55;
        raw[QStringLiteral("file")] = file;
        raw[QStringLiteral("preview")] = preview;
        raw[QStringLiteral("tags")] = tags;
        raw[QStringLiteral("rating")] = QStringLiteral("q");
        raw[QStringLiteral("score")] = score;
        raw[QStringLiteral("created_at")] =
            QStringLiteral("2026-10-31T02:52:13.501-04:00");
        raw[QStringLiteral("sources")] =
            QJsonArray{QStringLiteral("https://example.org/source")};
        const Post p = RemotePosts::postFromRaw(BooruEngine::Danbooru, raw,
                                               QStringLiteral("https://e621.net"));
        QCOMPARE(p.fileUrl, QStringLiteral("https://static.example/data/8a/97/x.jpg"));
        QCOMPARE(p.ext, QStringLiteral("jpg"));
        QCOMPARE(p.md5, QStringLiteral("8a977dc4717a72ddc8e5dc3cfd1fbd48"));
        QCOMPARE(p.width, 707);
        QCOMPARE(p.height, 1000);
        QCOMPARE(p.rating, QStringLiteral("questionable"));
        QCOMPARE(p.score, 7);
        QCOMPARE(p.tagsArtist, QStringLiteral("painter"));
        QCOMPARE(p.tagsCharacter, QStringLiteral("zangoose"));
        QCOMPARE(p.tagsCopyright, QStringLiteral("pokemon"));
        QCOMPARE(p.tagsGeneral, QStringLiteral("solo pokemon_(species)"));
        QCOMPARE(p.source, QStringLiteral("https://example.org/source"));
    }

    void straySidecarKeepsBuckets() {
        QJsonObject raw;
        raw[QStringLiteral("stray")] = true;
        raw[QStringLiteral("tags")] = QStringLiteral("1girl painter");
        raw[QStringLiteral("tags_artist")] = QStringLiteral("painter");
        raw[QStringLiteral("tags_general")] = QStringLiteral("1girl");
        raw[QStringLiteral("rating")] = QStringLiteral("general");
        raw[QStringLiteral("post_url")] = QStringLiteral("https://booru.example/post/1");
        Post p;
        QVERIFY(MetaParser::parse(raw, p));
        QCOMPARE(p.tagsArtist, QStringLiteral("painter"));
        QCOMPARE(p.tagsGeneral, QStringLiteral("1girl"));
        QCOMPARE(p.tags, QStringLiteral("painter 1girl"));
        QVERIFY(p.untyped.isEmpty());
        QCOMPARE(p.postUrl, QStringLiteral("https://booru.example/post/1"));
    }

    void straySidecarFlatTagsStillWork() {
        QJsonObject raw;
        raw[QStringLiteral("stray")] = true;
        raw[QStringLiteral("tags")] = QStringLiteral("hand tagged");
        Post p;
        QVERIFY(MetaParser::parse(raw, p));
        QCOMPARE(p.tagsGeneral, QStringLiteral("hand tagged"));
        QCOMPARE(p.untyped, QStringLiteral("hand tagged"));
    }

    void philomenaPostFromRaw() {
        // Stray resolve uses postFromRaw() for API results
        // (src/remote/strayresolve.cpp:480,513-517).
        QJsonObject derpi;
        derpi[QStringLiteral("id")] = 3926792;
        derpi[QStringLiteral("width")] = 100;
        derpi[QStringLiteral("height")] = 200;
        derpi[QStringLiteral("format")] = QStringLiteral("png");
        derpi[QStringLiteral("tags")] =
            QJsonArray{QStringLiteral("safe"), QStringLiteral("artist:test")};
        QJsonObject reps;
        reps[QStringLiteral("full")] =
            QStringLiteral("https://derpicdn.net/img/view/1.png");
        reps[QStringLiteral("thumb")] =
            QStringLiteral("https://derpicdn.net/img/thumb/1.png");
        derpi[QStringLiteral("representations")] = reps;
        const Post dp = RemotePosts::postFromRaw(
            BooruEngine::Derpibooru, derpi, QStringLiteral("https://derpibooru.org"));
        QVERIFY(dp.isRemote);
        QCOMPARE(dp.id, qint64(3926792));
        QCOMPARE(dp.postUrl, QStringLiteral("https://derpibooru.org/images/3926792"));

        QJsonObject twi;
        twi[QStringLiteral("id")] = 55;
        twi[QStringLiteral("width")] = 10;
        twi[QStringLiteral("height")] = 20;
        twi[QStringLiteral("format")] = QStringLiteral("jpg");
        twi[QStringLiteral("view_url")] = QStringLiteral("https://cdn.twibooru.org/x.jpg");
        twi[QStringLiteral("tags")] = QJsonArray{QStringLiteral("safe")};
        QJsonObject treps;
        treps[QStringLiteral("full")] = QStringLiteral("https://cdn.twibooru.org/x.jpg");
        treps[QStringLiteral("thumb")] = QStringLiteral("https://cdn.twibooru.org/t.jpg");
        twi[QStringLiteral("representations")] = treps;
        const Post tp = RemotePosts::postFromRaw(
            BooruEngine::Twibooru, twi, QStringLiteral("https://twibooru.org"));
        QVERIFY(tp.isRemote);
        QCOMPARE(tp.postUrl, QStringLiteral("https://twibooru.org/posts/55"));
    }

    void strayResolveHelpers() {
        // src/remote/strayresolve.cpp:282 (parseBooruPostUrl)
        auto r = RemotePosts::parseBooruPostUrl(QStringLiteral("https://gelbooru.com/index.php?page=post&s=view&id=123"));
        QCOMPARE(r.id, 123LL);
        QVERIFY(r.valid());
        auto r2 = RemotePosts::parseBooruPostUrl(QStringLiteral("https://gelbooru.com/index.php?page=post&s=view&id=0"));
        QCOMPARE(r2.id, 0LL);
        QVERIFY(!r2.valid());
        auto r3 = RemotePosts::parseBooruPostUrl(QStringLiteral("https://gelbooru.com/"));
        QCOMPARE(r3.id, 0LL);
        QVERIFY(!r3.valid());
    }
};

QTEST_MAIN(TestStrayResolve)
#include "test_strayresolve.moc"
