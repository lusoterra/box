// Read-only LAN Danbooru API: routing, JSON, media and thumbnails.
#include "sharecatalog.h"
#include "shareserver.h"

#include <QApplication>
#include <QEventLoop>
#include <QFile>
#include <QHostAddress>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

namespace {
struct Resp {
    int status = 0;
    QByteArray type;
    QByteArray body;
    QByteArray head;
};

Resp http(int port, const QByteArray &target,
          const QByteArray &extra = {}, QByteArray method = "GET") {
    QTcpSocket socket;
    QByteArray all;
    QEventLoop loop;
    QObject::connect(&socket, &QTcpSocket::readyRead, &socket,
                     [&socket, &all] { all += socket.readAll(); });
    QObject::connect(&socket, &QTcpSocket::disconnected, &loop,
                     &QEventLoop::quit);
    QTimer::singleShot(8000, &loop, &QEventLoop::quit);
    socket.connectToHost(QHostAddress::LocalHost, static_cast<quint16>(port));
    if (!socket.waitForConnected(2000))
        return {};
    const QByteArray req =
        method + ' ' + target + " HTTP/1.1\r\nHost: 127.0.0.1:" +
        QByteArray::number(port) + "\r\nConnection: close\r\n" + extra +
        "\r\n";
    socket.write(req);
    loop.exec();
    all += socket.readAll();
    const int headEnd = all.indexOf("\r\n\r\n");
    if (headEnd < 0)
        return {};
    Resp r;
    r.head = all.left(headEnd);
    r.body = all.mid(headEnd + 4);
    const QList<QByteArray> lines = r.head.split('\n');
    if (!lines.isEmpty()) {
        const QList<QByteArray> first = lines.first().trimmed().split(' ');
        if (first.size() >= 2)
            r.status = first.at(1).toInt();
    }
    for (const QByteArray &line : lines) {
        const QByteArray t = line.trimmed();
        if (t.toLower().startsWith("content-type:"))
            r.type = t.mid(13).trimmed();
    }
    return r;
}
} // namespace

class TestShareServer : public QObject {
    Q_OBJECT
private slots:
    void postsTagsCountsMediaAndThumbs() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString imagePath = dir.filePath(QStringLiteral("pic.png"));
        QImage img(80, 60, QImage::Format_RGB32);
        img.fill(QColor(180, 60, 60));
        QVERIFY(img.save(imagePath));

        Post p;
        p.absFile = imagePath;
        p.file = imagePath;
        p.ext = QStringLiteral("png");
        p.tags = QStringLiteral("alpha beta alice");
        p.tagsGeneral = QStringLiteral("alpha beta");
        p.tagsArtist = QStringLiteral("alice");
        p.rating = QStringLiteral("general");
        p.date = QStringLiteral("2026-11-24T12:00:00Z");
        p.md5 = QStringLiteral("0123456789abcdef0123456789abcdef");
        p.siteName = QStringLiteral("Local");
        p.width = 80;
        p.height = 60;

        auto catalog = std::make_shared<ShareCatalog>();
        QVERIFY(catalog->add(p, imagePath));
        const qint64 id = catalog->items().first().id;

        ShareServer server;
        server.setCatalog(catalog);
        QString error;
        QVERIFY2(server.start(QStringLiteral("127.0.0.1"), 0,
                              dir.filePath(QStringLiteral("thumbs")), &error),
                 qPrintable(error));
        const int port = server.port();

        const Resp list = http(port, "/posts.json?tags=alpha&limit=10");
        QCOMPARE(list.status, 200);
        const QJsonDocument listDoc = QJsonDocument::fromJson(list.body);
        QVERIFY(listDoc.isArray());
        QCOMPARE(listDoc.array().size(), 1);
        const QJsonObject post = listDoc.array().first().toObject();
        QCOMPARE(post.value(QStringLiteral("id")).toInteger(), id);
        QCOMPARE(post.value(QStringLiteral("tag_string_artist")).toString(),
                 QStringLiteral("alice"));
        QVERIFY(post.value(QStringLiteral("file_url"))
                    .toString()
                    .contains(QStringLiteral("/share/file/")));

        const Resp detail =
            http(port, "/posts/" + QByteArray::number(id) + ".json");
        QCOMPARE(detail.status, 200);
        QCOMPARE(QJsonDocument::fromJson(detail.body)
                     .object()
                     .value(QStringLiteral("id"))
                     .toInteger(),
                 id);

        const Resp tags =
            http(port, "/tags.json?search[name_matches]=ali*&limit=20");
        QCOMPARE(tags.status, 200);
        const QJsonArray tagArr = QJsonDocument::fromJson(tags.body).array();
        QCOMPARE(tagArr.size(), 1);
        QCOMPARE(tagArr.first().toObject()
                     .value(QStringLiteral("category"))
                     .toInt(),
                 1);

        const Resp counts = http(port, "/counts/posts.json?tags=alpha");
        QCOMPARE(counts.status, 200);
        QCOMPARE(QJsonDocument::fromJson(counts.body)
                     .object()
                     .value(QStringLiteral("counts"))
                     .toObject()
                     .value(QStringLiteral("posts"))
                     .toInt(),
                 1);

        const Resp media =
            http(port, "/share/file/" + QByteArray::number(id));
        QCOMPARE(media.status, 200);
        QCOMPARE(media.type, QByteArray("image/png"));
        QVERIFY(media.body.size() > 8);
        const Resp range =
            http(port, "/share/file/" + QByteArray::number(id),
                 "Range: bytes=0-3\r\n");
        QCOMPARE(range.status, 206);
        QCOMPARE(range.body.size(), 4);

        const Resp thumb =
            http(port, "/share/thumb/" + QByteArray::number(id));
        QCOMPARE(thumb.status, 200);
        QCOMPARE(thumb.type, QByteArray("image/jpeg"));
        QVERIFY(!thumb.body.isEmpty());

        QCOMPARE(http(port, "/nope").status, 404);
        QCOMPARE(http(port, "/posts.json", {}, "POST").status, 405);

        const Resp head = http(
            port, "/share/file/" + QByteArray::number(id), {}, "HEAD");
        QCOMPARE(head.status, 200);
        QVERIFY(head.body.isEmpty());
        server.stop();
    }

    void rootServesTheSharePage() {
        ShareServer server;
        server.setCatalog(std::make_shared<ShareCatalog>());
        QString error;
        QVERIFY2(server.start(QStringLiteral("127.0.0.1"), 0, {}, &error),
                 qPrintable(error));
        const Resp root = http(server.port(), "/");
        QCOMPARE(root.status, 200);
        QVERIFY(root.type.startsWith("text/html"));
        QVERIFY(!root.body.isEmpty());
        QVERIFY(root.body.toLower().contains("box"));
        if (QFile::exists(QStringLiteral(":/share/index.html")))
            QVERIFY(server.hasWebUi());
        const Resp index = http(server.port(), "/index.html");
        QCOMPARE(index.status, 200);
        QCOMPARE(index.body, root.body);
        server.stop();
    }

    void emptyCatalogIsSafe() {
        ShareServer server;
        server.setCatalog(std::make_shared<ShareCatalog>());
        QString error;
        QVERIFY(server.start(QStringLiteral("127.0.0.1"), 0, {}, &error));
        const Resp r = http(server.port(), "/posts.json");
        QCOMPARE(r.status, 200);
        QCOMPARE(QJsonDocument::fromJson(r.body).array().size(), 0);
        server.stop();
    }
};

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    TestShareServer tc;
    return QTest::qExec(&tc, argc, argv);
}
#include "test_shareserver.moc"
