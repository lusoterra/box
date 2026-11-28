// Engine auto-detect signatures: each family is identified from its own
// probe body without touching the network.
#include "enginedetect.h"

#include <QEventLoop>
#include <QHash>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QString>
#include <QTimer>
#include <QtTest>

class TestEngineDetect : public QObject {
    Q_OBJECT
private slots:
    void detectsEShuushuu() {
        const QHash<QString, QByteArray> bodies = {
            {QStringLiteral("https://e-shuushuu.net/api/v1/images?per_page=1"),
             "{\"total\":1092385,\"page\":1,\"per_page\":20,\"images\":[{\"image_id\":1}]}"}};
        const auto r = EngineDetect::detect(
            QStringLiteral("https://e-shuushuu.net"),
            [&](const QString &url) { return bodies.value(url); });
        QVERIFY(r.found);
        QVERIFY(r.engine == BooruEngine::EShuushuu);
        QCOMPARE(r.baseUrl, QStringLiteral("https://e-shuushuu.net"));
    }

    void detectsE621BeforeDanbooru() {
        const QHash<QString, QByteArray> bodies = {
            {QStringLiteral("https://e621.net/posts.json?limit=1"),
             "[{\"id\":1,\"files\":{\"original\":{\"url\":\"https://x/1.png\"}},\"tags\":{}}]"}};
        auto r = EngineDetect::detect(
            QStringLiteral("https://e621.net"),
            [&](const QString &url) { return bodies.value(url); });
        QVERIFY(r.found);
        QVERIFY(r.engine == BooruEngine::E621);
    }

    void detectsDanbooru() {
        const QHash<QString, QByteArray> bodies = {
            {QStringLiteral("https://danbooru.donmai.us/posts.json?limit=1"),
             "[{\"id\":1,\"tag_string_general\":\"solo\",\"tag_string_artist\":\"alice\"}]"}};
        auto r = EngineDetect::detect(
            QStringLiteral("https://danbooru.donmai.us"),
            [&](const QString &url) { return bodies.value(url); });
        QVERIFY(r.found);
        QVERIFY(r.engine == BooruEngine::Danbooru);
    }

    void detectsAnimePictures() {
        const QHash<QString, QByteArray> bodies = {
            {QStringLiteral("https://anime-pictures.net/api/v3/posts?page=0"
                            "&posts_per_page=1&lang=en"),
             "{\"posts\":[{\"id\":1,\"md5\":\"0123456789abcdef0123456789abcdef\",\"ext\":\".jpg\"}]}"}};
        auto r = EngineDetect::detect(
            QStringLiteral("https://anime-pictures.net"),
            [&](const QString &url) { return bodies.value(url); });
        QVERIFY(r.found);
        QVERIFY(r.engine == BooruEngine::AnimePictures);
    }

    void detectsHybooru() {
        const QHash<QString, QByteArray> bodies = {
            {QStringLiteral("https://hy.example/api/post?page=0&pageSize=1"),
             "{\"posts\":[{\"id\":1,\"sha256\":\"abc\",\"extension\":\".png\"}]}"}};
        auto r = EngineDetect::detect(
            QStringLiteral("https://hy.example"),
            [&](const QString &url) { return bodies.value(url); });
        QVERIFY(r.found);
        QVERIFY(r.engine == BooruEngine::Hybooru);
    }

    void detectsPhilomenaAndBooruOnRails() {
        {
            const QHash<QString, QByteArray> bodies = {
                {QStringLiteral("https://derpibooru.org/api/v1/json/search/images"
                                "?q=%2A&page=1&per_page=1"),
                 "{\"total\":1,\"images\":[{\"id\":1}]}"}};
            auto r = EngineDetect::detect(
                QStringLiteral("https://derpibooru.org"),
                [&](const QString &url) { return bodies.value(url); });
            QVERIFY(r.found);
            QVERIFY(r.engine == BooruEngine::Philomena);
        }
        {
            const QHash<QString, QByteArray> bodies = {
                {QStringLiteral("https://twibooru.org/api/v3/search/posts"
                                "?q=%2A&page=1&per_page=1"),
                 "{\"posts\":[{\"id\":1}]}"}};
            auto r = EngineDetect::detect(
                QStringLiteral("https://twibooru.org"),
                [&](const QString &url) { return bodies.value(url); });
            QVERIFY(r.found);
            QVERIFY(r.engine == BooruEngine::BooruOnRails);
        }
    }

    void detectsShimmieAndSzuru() {
        {
            const QHash<QString, QByteArray> bodies = {
                {QStringLiteral("https://shim.example/api/danbooru/post/"
                                "index.xml?limit=1"),
                 "<posts><post id=\"1\"/></posts>"}};
            auto r = EngineDetect::detect(
                QStringLiteral("https://shim.example"),
                [&](const QString &url) { return bodies.value(url); });
            QVERIFY(r.found);
            QVERIFY(r.engine == BooruEngine::Shimmie);
        }
        {
            const QHash<QString, QByteArray> bodies = {
                {QStringLiteral("https://szu.example/api/info"),
                 "{\"version\":\"2026\",\"serverTime\":\"now\"}"}};
            auto r = EngineDetect::detect(
                QStringLiteral("https://szu.example"),
                [&](const QString &url) { return bodies.value(url); });
            QVERIFY(r.found);
            QVERIFY(r.engine == BooruEngine::Szurubooru);
        }
    }

    void detectsMoebooruGelbooruNuuru() {
        {
            const QHash<QString, QByteArray> bodies = {
                {QStringLiteral("https://yande.re/post.json?limit=1"),
                 "[{\"id\":1,\"tags\":\"solo\"}]"}};
            auto r = EngineDetect::detect(
                QStringLiteral("https://yande.re"),
                [&](const QString &url) { return bodies.value(url); });
            QVERIFY(r.found);
            QVERIFY(r.engine == BooruEngine::Moebooru);
        }
        {
            const QHash<QString, QByteArray> bodies = {
                {QStringLiteral("https://gel.example/index.php?page=dapi&s=post"
                                "&q=index&limit=1&json=1"),
                 "{\"post\":[{\"id\":1,\"tags\":\"solo\"}]}"}};
            auto r = EngineDetect::detect(
                QStringLiteral("https://gel.example"),
                [&](const QString &url) { return bodies.value(url); });
            QVERIFY(r.found);
            QVERIFY(r.engine == BooruEngine::Gelbooru);
        }
        {
            const QHash<QString, QByteArray> bodies = {
                {QStringLiteral("https://soy.example/api/booru/posts?limit=1"),
                 "{\"error\":\"pow_required\"}"}};
            auto r = EngineDetect::detect(
                QStringLiteral("https://soy.example"),
                [&](const QString &url) { return bodies.value(url); });
            QVERIFY(r.found);
            QVERIFY(r.engine == BooruEngine::Nuuru);
        }
    }

    void knownHostsSkipProbes() {
        int calls = 0;
        const auto fetch = [&](const QString &) {
            ++calls;
            return QByteArray();
        };
        const auto check = [&](const char *url, BooruEngine want) {
            const auto r = EngineDetect::detect(QString::fromLatin1(url), fetch);
            QVERIFY2(r.found, url);
            QCOMPARE(int(r.engine), int(want));
        };
        check("https://danbooru.donmai.us", BooruEngine::Danbooru);
        check("https://safebooru.donmai.us", BooruEngine::Danbooru);
        check("https://aibooru.online", BooruEngine::Danbooru);
        check("https://gelbooru.com", BooruEngine::Gelbooru);
        check("https://safebooru.org", BooruEngine::Gelbooru);
        check("https://rule34.xxx", BooruEngine::Gelbooru);
        check("https://realbooru.com", BooruEngine::Gelbooru);
        check("https://yande.re", BooruEngine::Moebooru);
        check("https://konachan.net", BooruEngine::Moebooru);
        check("https://e621.net", BooruEngine::E621);
        check("https://e926.net", BooruEngine::E621);
        check("https://anime-pictures.net", BooruEngine::AnimePictures);
        check("https://e-shuushuu.net", BooruEngine::EShuushuu);
        check("https://derpibooru.org", BooruEngine::Philomena);
        check("https://twibooru.org", BooruEngine::BooruOnRails);
        check("https://rule34.paheal.net", BooruEngine::Shimmie);
        check("https://soybooru.com", BooruEngine::Nuuru);
        check("https://visuabusters.com", BooruEngine::Szurubooru);
        QCOMPARE(EngineDetect::detect(
                     QStringLiteral("https://visuabusters.com"),
                     [](const QString &) { return QByteArray(); })
                     .baseUrl,
                 QStringLiteral("https://visuabusters.com/booru"));
        QCOMPARE(calls, 0); // known hosts never touch the network
    }

    void schemeLessKnownHost() {
        const auto r = EngineDetect::detect(
            QStringLiteral("gelbooru.com/"),
            [](const QString &) { return QByteArray(); });
        QVERIFY(r.found);
        QVERIFY(r.engine == BooruEngine::Gelbooru);
        QCOMPARE(r.baseUrl, QStringLiteral("https://gelbooru.com"));
    }

    void liveDetectIfRequested() {
        if (!qEnvironmentVariableIsSet("BOX_LIVE_DETECT"))
            QSKIP("set BOX_LIVE_DETECT=1 for the live probe");
        QNetworkAccessManager nam;
        const auto get = [&](const QString &url) -> QByteArray {
            QNetworkRequest req{QUrl(url)};
            req.setTransferTimeout(7000);
            req.setHeader(QNetworkRequest::UserAgentHeader,
                          QStringLiteral("box/0.14 (engine detect)"));
            req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                             QNetworkRequest::NoLessSafeRedirectPolicy);
            QNetworkReply *reply = nam.get(req);
            QEventLoop loop;
            QTimer timer;
            timer.setSingleShot(true);
            QObject::connect(&timer, &QTimer::timeout, &loop,
                             &QEventLoop::quit);
            QObject::connect(reply, &QNetworkReply::finished, &loop,
                             &QEventLoop::quit);
            timer.start(8000);
            loop.exec();
            timer.stop();
            const QByteArray body =
                reply->error() == QNetworkReply::NoError ? reply->readAll()
                                                         : QByteArray();
            reply->deleteLater();
            return body;
        };
        const auto r = EngineDetect::detect(
            QStringLiteral("https://e-shuushuu.net"), get);
        QVERIFY(r.found);
        QVERIFY(r.engine == BooruEngine::EShuushuu);
    }

    void unknownHostStaysUnknown() {
        const auto r = EngineDetect::detect(
            QStringLiteral("https://nothing.example"),
            [](const QString &) { return QByteArray(); });
        QVERIFY(!r.found);
    }
};

QTEST_GUILESS_MAIN(TestEngineDetect)
#include "test_enginedetect.moc"
