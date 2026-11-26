// e-shuushuu engine: tag-ID search URL, tag-name resolution and the
// image object mapper.
#include "booruengine.h"
#include "remotesource.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrlQuery>
#include <QtTest>

namespace {
const QString kBase = QStringLiteral("https://e-shuushuu.net");

QByteArray imageFixture() {
    return QByteArray(R"JSON({"filename":"2026-11-25-1123799","ext":"jpg","original_filename":"x.jpg","md5_hash":"8174c32c7b3098374de7b9bc8b908cfd","filesize":9894157,"width":2163,"height":3670,"caption":"","source_url":"https://x/1","status":1,"rating":0.0,"image_id":1123799,"favorites":3,"date_added":"2026-11-26T00:08:24Z","url":"https://cdn.e-shuushuu.net/fullsize/2026-11-25-1123799.jpg","thumbnail_url":"https://cdn.e-shuushuu.net/thumbs/2026-11-25-1123799.webp","medium_url":"https://cdn.e-shuushuu.net/medium/2026-11-25-1123799.jpg","large_url":"https://cdn.e-shuushuu.net/large/2026-11-25-1123799.jpg","tags":[{"tag_id":239897,"title":"GOOTAI","type":3,"type_name":"Artist"},{"tag_id":270,"title":"Vampire Princess Miyu","type":2,"type_name":"Source"},{"tag_id":75537,"title":"Higurashi Kagome","type":4,"type_name":"Character"},{"tag_id":46,"title":"long hair","type":1,"type_name":"Theme"}]})JSON");
}

QByteArray searchFixture() {
    return QByteArray(R"JSON({"total":1,"page":1,"per_page":20,"images":[{"filename":"2026-11-25-1123799","ext":"jpg","md5_hash":"8174c32c7b3098374de7b9bc8b908cfd","filesize":10,"width":10,"height":20,"image_id":1123799,"favorites":2,"url":"https://cdn/full.jpg","thumbnail_url":"https://cdn/thumb.webp","large_url":"https://cdn/large.jpg","tags":[{"tag_id":46,"title":"long hair","type":1,"type_name":"Theme"}]}]})JSON");
}
} // namespace

class TestShuushuu : public QObject {
    Q_OBJECT
private slots:
    void mapsImage() {
        bool ok = false;
        const BooruEngineOps *ops = engineOps(BooruEngine::EShuushuu);
        const QJsonObject raw = ops->parseSingle(imageFixture(), ok);
        QVERIFY(ok);
        const Post p = RemotePosts::fromEshuushuu(raw, kBase);
        QCOMPARE(p.id, 1123799LL);
        QCOMPARE(p.ext, QStringLiteral("jpg"));
        QCOMPARE(p.width, 2163);
        QCOMPARE(p.height, 3670);
        QCOMPARE(p.score, 3);
        QCOMPARE(p.md5, QStringLiteral("8174c32c7b3098374de7b9bc8b908cfd"));
        QCOMPARE(p.rating, QStringLiteral("general"));
        QCOMPARE(p.fileUrl,
                 QStringLiteral("https://cdn.e-shuushuu.net/fullsize/"
                                "2026-11-25-1123799.jpg"));
        QCOMPARE(p.previewUrl,
                 QStringLiteral("https://cdn.e-shuushuu.net/thumbs/"
                                "2026-11-25-1123799.webp"));
        QCOMPARE(p.sampleUrl,
                 QStringLiteral("https://cdn.e-shuushuu.net/large/"
                                "2026-11-25-1123799.jpg"));
        QCOMPARE(p.postUrl, kBase + QStringLiteral("/images/1123799"));
        QCOMPARE(p.source, QStringLiteral("https://x/1"));
        QVERIFY(p.tagsArtist.split(u' ').contains(QStringLiteral("GOOTAI")));
        QVERIFY(p.tagsCopyright.split(u' ').contains(
            QStringLiteral("Vampire_Princess_Miyu")));
        QVERIFY(p.tagsCharacter.split(u' ').contains(
            QStringLiteral("Higurashi_Kagome")));
        QVERIFY(p.tagsGeneral.split(u' ').contains(QStringLiteral("long_hair")));
        QCOMPARE(p.siteKey, QStringLiteral(""));
        QVERIFY(p.isRemote && p.hasMeta);
    }

    void parsesSearchAndTags() {
        const BooruEngineOps *ops = engineOps(BooruEngine::EShuushuu);
        const SearchResult r = ops->parseSearch(
            QStringLiteral("e-shuushuu"),
            {BooruEngine::EShuushuu, kBase, {}}, searchFixture());
        QVERIFY(r.ok());
        QCOMPARE(r.posts.size(), 1);
        QCOMPARE(r.posts.first().id, 1123799LL);
        QCOMPARE(r.posts.first().siteKey, QStringLiteral("e-shuushuu"));
        QVERIFY(r.posts.first().tagsGeneral.contains(QStringLiteral("long_hair")));

        const QByteArray tags =
            R"({"total":2,"tags":[)"
            R"({"tag_id":46,"title":"long hair","type":1,"alias_of":null},)"
            R"({"tag_id":5,"title":"longhair","type":1,"alias_of":46}]})";
        QCOMPARE(ops->parseTagId(tags, QStringLiteral("long hair")), 46LL);
        QCOMPARE(ops->parseTagId(tags, QStringLiteral("longhair")), 46LL);
        QCOMPARE(ops->parseTagId(tags, QStringLiteral("nope")), 0LL);
    }

    void buildsIdSearchUrl() {
        const BooruEngineOps *ops = engineOps(BooruEngine::EShuushuu);
        const BooruEngineOps::Context ctx{BooruEngine::EShuushuu, kBase, {}};
        const QUrl url = ops->resolvedSearchUrl(
            ctx, {46, 1}, {2}, 3, 40);
        const QUrlQuery q(url.query());
        QCOMPARE(q.queryItemValue(QStringLiteral("tags")),
                 QStringLiteral("46,1"));
        QCOMPARE(q.queryItemValue(QStringLiteral("exclude_tags")),
                 QStringLiteral("2"));
        QCOMPARE(q.queryItemValue(QStringLiteral("tags_mode")),
                 QStringLiteral("all"));
        QCOMPARE(q.queryItemValue(QStringLiteral("status")),
                 QStringLiteral("1"));
        QCOMPARE(q.queryItemValue(QStringLiteral("page")), QStringLiteral("3"));
        QCOMPARE(q.queryItemValue(QStringLiteral("per_page")),
                 QStringLiteral("40"));
        QCOMPARE(q.queryItemValue(QStringLiteral("sort_by")),
                 QStringLiteral("image_id"));
        QCOMPARE(ops->refreshUrl(ctx, 1123799).toString(),
                 kBase + QStringLiteral("/api/v1/images/1123799/"));
    }

    void preflightSearchResolvesTagNames() {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        QStringList paths;
        QObject::connect(&server, &QTcpServer::newConnection, &server, [&] {
            while (QTcpSocket *socket = server.nextPendingConnection()) {
                QObject::connect(socket, &QTcpSocket::readyRead, socket,
                                 [&, socket] {
                                     const QByteArray request = socket->readAll();
                                     const QByteArray target =
                                         request.split('\n').value(0).split(' ')
                                             .value(1);
                                     paths << QString::fromUtf8(target);
                                     QByteArray body;
                                     if (target.startsWith("/api/v1/tags"))
                                         body = R"({"total":1,"tags":[)"
                                                R"({"tag_id":46,"title":"long hair",)"
                                                R"("type":1,"alias_of":null}]})";
                                     else if (target.startsWith("/api/v1/images"))
                                         body = searchFixture();
                                     else
                                         body = "{}";
                                     const QByteArray response =
                                         "HTTP/1.1 200 OK\r\n"
                                         "Content-Type: application/json\r\n"
                                         "Content-Length: " +
                                         QByteArray::number(body.size()) +
                                         "\r\nConnection: close\r\n\r\n" +
                                         body;
                                     socket->write(response);
                                     socket->disconnectFromHost();
                                 });
            }
        });
        BooruSite site;
        site.key = QStringLiteral("shu-test");
        site.label = QStringLiteral("e-shuushuu");
        site.baseUrl = QStringLiteral("http://127.0.0.1:%1")
                           .arg(server.serverPort());
        site.engine = BooruEngine::EShuushuu;
        RemoteSource source(site);
        SearchResult got;
        QEventLoop loop;
        QObject::connect(&source, &RemoteSource::searchFinished, &loop,
                         [&](const SearchResult &result) {
                             got = result;
                             loop.quit();
                         });
        QTimer::singleShot(5000, &loop, &QEventLoop::quit);
        source.search({QStringLiteral("long_hair")}, 1, 2);
        loop.exec();
        QVERIFY2(got.ok(), qPrintable(got.error));
        QCOMPARE(got.posts.size(), 1);
        QCOMPARE(got.posts.first().id, 1123799LL);
        const QUrlQuery last(source.lastUrl());
        QVERIFY(source.lastUrl().path().endsWith(QStringLiteral("/api/v1/images")));
        QCOMPARE(last.queryItemValue(QStringLiteral("tags")),
                 QStringLiteral("46"));
        QCOMPARE(last.queryItemValue(QStringLiteral("page")),
                 QStringLiteral("1"));
        QCOMPARE(last.queryItemValue(QStringLiteral("per_page")),
                 QStringLiteral("2"));
        // One tag lookup, then the image search.
        QCOMPARE(paths.size(), 2);
        QVERIFY(paths.first().startsWith("/api/v1/tags"));
        QVERIFY(paths.at(1).startsWith("/api/v1/images"));
    }

    void liveSearchIfRequested() {
        if (!qEnvironmentVariableIsSet("BOX_LIVE_SHUSHUU"))
            QSKIP("set BOX_LIVE_SHUSHUU=1 for the live API probe");
        BooruSite site;
        site.key = QStringLiteral("e-shuushuu");
        site.label = QStringLiteral("e-shuushuu");
        site.baseUrl = QStringLiteral("https://e-shuushuu.net");
        site.engine = BooruEngine::EShuushuu;
        RemoteSource source(site);
        SearchResult got;
        QEventLoop loop;
        QObject::connect(&source, &RemoteSource::searchFinished, &loop,
                         [&](const SearchResult &r) {
                             got = r;
                             loop.quit();
                         });
        QTimer::singleShot(20000, &loop, &QEventLoop::quit);
        source.search({QStringLiteral("long_hair")}, 1, 5);
        loop.exec();
        QVERIFY2(got.ok(), qPrintable(got.error));
        QVERIFY(!got.posts.isEmpty());
        QVERIFY(got.posts.first().fileUrl.startsWith(
            QStringLiteral("https://")));
        QVERIFY(!got.posts.first().previewUrl.isEmpty());
        QVERIFY(!got.posts.first().tagsGeneral.isEmpty());
    }

    void engineMapping() {
        QCOMPARE(RemotePosts::engineKey(BooruEngine::EShuushuu),
                 QStringLiteral("e-shuushuu"));
        QVERIFY(RemotePosts::engineForKey(QStringLiteral("e-shuushuu")) ==
                BooruEngine::EShuushuu);
        QVERIFY(RemotePosts::engineForKey(QStringLiteral("shuushuu")) ==
                BooruEngine::EShuushuu);
        QCOMPARE(RemotePosts::engineLabel(BooruEngine::EShuushuu),
                 QStringLiteral("e-shuushuu"));
        QCOMPARE(RemotePosts::postPageUrlFor(
                     BooruSite{QStringLiteral("e-shuushuu"),
                               QStringLiteral("e-shuushuu"),
                               QStringLiteral("SHU"), {},
                               kBase, BooruEngine::EShuushuu},
                     1123799),
                 kBase + QStringLiteral("/images/1123799"));
    }
};

QTEST_GUILESS_MAIN(TestShuushuu)
#include "test_shuushuu.moc"
