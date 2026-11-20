// ThumbLoader: dedicated pool, scroll-spam dedupe (one task per key),
// all exit paths resolve. Headless-safe (QImage only, local files).
#include "tempmedia.h"
#include "thumbloader.h"
#include "webdlbackend.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QHostAddress>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>

#include <atomic>
#include <utility>

namespace {
// First real jpg in the repo's sample dump (whatever it is called).
QString sampleJpg() {
    const QStringList jpgs =
        QDir(QString::fromLatin1(TESTFILES_DIR))
            .entryList({QStringLiteral("*.jpg")}, QDir::Files);
    return jpgs.isEmpty()
               ? QString()
               : QString::fromLatin1(TESTFILES_DIR) + u'/' + jpgs.first();
}

QByteArray sampleJpgBytes() {
    QFile f(sampleJpg());
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

// Tiny local HTTP/1.1 server: one canned response per connection. No
// real network (127.0.0.1 only), so the test stays hermetic.
class CannedHttpServer : public QTcpServer {
public:
    CannedHttpServer(QByteArray status, QByteArray type, QByteArray body)
        : m_status(std::move(status)), m_type(std::move(type)),
          m_body(std::move(body)) {
        listen(QHostAddress::LocalHost);
    }
    QString url(const QString &path = QStringLiteral("/thumb.jpg")) const {
        return QStringLiteral("http://127.0.0.1:%1%2")
            .arg(serverPort())
            .arg(path);
    }

protected:
    void incomingConnection(qintptr fd) override {
        auto *s = new QTcpSocket(this);
        s->setSocketDescriptor(fd);
        connect(s, &QTcpSocket::readyRead, s, [this, s] {
            s->readAll(); // request is irrelevant: one canned answer
            QByteArray resp = "HTTP/1.1 " + m_status +
                              "\r\nContent-Type: " + m_type +
                              "\r\nContent-Length: " +
                              QByteArray::number(m_body.size()) +
                              "\r\nConnection: close\r\n\r\n" + m_body;
            s->write(resp);
            s->disconnectFromHost();
        });
        connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
    }

private:
    QByteArray m_status;
    QByteArray m_type;
    QByteArray m_body;
};

class FakeWebBackend : public WebDownloadBackend {
public:
    FakeWebBackend(QByteArray bytes, bool ok)
        : m_bytes(std::move(bytes)), m_ok(ok) {}
    void start(qint64, const QString &, const QString &) override {}
    void cancel(qint64) override {}
    bool fetchBlocking(const QString &, const QString &destPath,
                       QString *error, int) override {
        ++calls;
        if (!m_ok) {
            if (error)
                *error = QStringLiteral("Server returned a page, not media");
            return false;
        }
        QFile f(destPath);
        if (!f.open(QIODevice::WriteOnly))
            return false;
        f.write(m_bytes);
        return true;
    }

    std::atomic<int> calls{0};

private:
    QByteArray m_bytes;
    bool m_ok;
};

const QByteArray kChallengeHtml =
    QByteArrayLiteral("<!DOCTYPE html><html><head><title>Just a moment..."
                      "</title></head><body>challenges.cloudflare.com"
                      "</body></html>");
} // namespace

class TestThumbLoader : public QObject {
    Q_OBJECT
private slots:
    void duplicateRequestsCollapse() {
        QVERIFY(!sampleJpg().isEmpty());
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        ThumbLoader loader;
        loader.setAspect(1, 1);
        loader.setCacheDir(tmp.path());
        QSignalSpy ready(&loader, &ThumbLoader::thumbnailReady);
        QVERIFY(ready.isValid());
        // Five back-to-back requests for the same key: one task, one signal.
        for (int i = 0; i < 5; ++i)
            loader.request(QStringLiteral("same"), sampleJpg(),
                           0, 0, 128);
        QVERIFY(ready.wait(15000));
        // Let any strays land, then assert exactly one delivery.
        QTest::qWait(1500);
        QCOMPARE(ready.size(), 1);
        QVERIFY(!ready.first().at(1).value<QImage>().isNull());
    }

    void distinctKeysAllResolve() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        ThumbLoader loader;
        loader.setAspect(1, 1);
        loader.setCacheDir(tmp.path());
        QSignalSpy ready(&loader, &ThumbLoader::thumbnailReady);
        QVERIFY(ready.isValid());
        loader.request(QStringLiteral("a"), sampleJpg(), 0, 0, 128);
        loader.request(QStringLiteral("b"), sampleJpg(), 1, 0, 128);
        QVERIFY(ready.wait(15000));
        QTest::qWait(1500);
        QCOMPARE(ready.size(), 2);
    }
    void cancelIsSafe() {
        // Quit path: cancelling an idle loader is a no-op (never crashes),
        // and a cancelled loader accepts fresh requests afterwards.
        QVERIFY(!sampleJpg().isEmpty());
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        ThumbLoader loader;
        loader.setAspect(1, 1);
        loader.setCacheDir(tmp.path());
        loader.cancel();
        QSignalSpy ready(&loader, &ThumbLoader::thumbnailReady);
        QVERIFY(ready.isValid());
        loader.request(QStringLiteral("after-cancel"), sampleJpg(), 0, 0, 128);
        QVERIFY(ready.wait(15000));
        QCOMPARE(ready.size(), 1);
    }

    void progressSignalsTrackTheTask() {
        // The grid's per-card veil needs queued -> started -> ready (and
        // never a stuck queued state) even on a fast local response.
        const QByteArray jpg = sampleJpgBytes();
        QVERIFY(!jpg.isEmpty());
        CannedHttpServer server("200 OK", "image/jpeg", jpg);
        ThumbLoader loader;
        loader.setAspect(1, 1);
        QSignalSpy queued(&loader, &ThumbLoader::thumbnailQueued);
        QSignalSpy started(&loader, &ThumbLoader::thumbnailStarted);
        QSignalSpy failed(&loader, &ThumbLoader::thumbnailFailed);
        QSignalSpy ready(&loader, &ThumbLoader::thumbnailReady);
        QVERIFY(queued.isValid());
        QVERIFY(started.isValid());
        QVERIFY(failed.isValid());
        loader.request(QStringLiteral("url|progress"), server.url(), 0, 0, 128);
        QCOMPARE(queued.size(), 1); // synchronous accept
        QVERIFY(ready.wait(15000));
        QCOMPARE(started.size(), 1);
        QCOMPARE(failed.size(), 0);
        QCOMPARE(ready.size(), 1);
    }

    void plainWallRetriesThroughBrowser() {
        // A non-quirk host (the Danbooru/CDN case) answers HTML instead
        // of the thumbnail: the loader must ride the browser backend and
        // still deliver the image, not leave a black card.
        const QByteArray jpg = sampleJpgBytes();
        QVERIFY(!jpg.isEmpty());
        CannedHttpServer server("403 Forbidden", "text/html", kChallengeHtml);
        FakeWebBackend web(jpg, true);
        ThumbLoader loader;
        loader.setAspect(1, 1);
        loader.setWebBackend(&web);
        QSignalSpy ready(&loader, &ThumbLoader::thumbnailReady);
        QSignalSpy wall(&loader, &ThumbLoader::thumbnailWall);
        loader.request(QStringLiteral("url|wall"), server.url(), 0, 0, 128,
                       {}, false, false, QStringLiteral("danbooru"));
        QVERIFY(ready.wait(15000));
        QCOMPARE(ready.size(), 1);
        QVERIFY(!ready.first().at(1).value<QImage>().isNull());
        QVERIFY(web.calls.load() >= 1);
        // The shell hears about the wall immediately (starts the capture
        // pass while the browser attempt runs), then the image lands.
        QCOMPARE(wall.size(), 1);
    }

    void wallSignalWhenBrowserCannotPass() {
        CannedHttpServer server("403 Forbidden", "text/html", kChallengeHtml);
        FakeWebBackend web({}, false);
        ThumbLoader loader;
        loader.setAspect(1, 1);
        loader.setWebBackend(&web);
        QSignalSpy ready(&loader, &ThumbLoader::thumbnailReady);
        QSignalSpy wall(&loader, &ThumbLoader::thumbnailWall);
        QSignalSpy failed(&loader, &ThumbLoader::thumbnailFailed);
        loader.request(QStringLiteral("url|still-walled"), server.url(), 0, 0,
                       128, {}, false, false, QStringLiteral("danbooru"));
        QVERIFY(wall.wait(15000));
        QCOMPARE(wall.size(), 1);
        QCOMPARE(wall.first().at(0).toString(), QStringLiteral("danbooru"));
        QCOMPARE(wall.first().at(1).toString(), server.url());
        QCOMPARE(ready.size(), 0);
        // The card must not keep spinning after the wall gives up.
        QTRY_COMPARE(failed.size(), 1);
        QCOMPARE(failed.first().at(0).toString(),
                 QStringLiteral("url|still-walled"));
    }

    void tempMediaRejectsChallengePage() {
        CannedHttpServer server("200 OK", "text/html", kChallengeHtml);
        TempMedia::Result got;
        bool done = false;
        TempMedia::ensureTemp(server.url(QStringLiteral("/x.jpg")),
                              QStringLiteral("jpg"),
                              [&](const TempMedia::Result &r) {
                                  got = r;
                                  done = true;
                              });
        QTRY_VERIFY_WITH_TIMEOUT(done, 15000);
        QVERIFY(got.path.isEmpty());
        QVERIFY(got.walled);
        QVERIFY(!got.error.isEmpty());
    }
};

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    TestThumbLoader tc;
    return QTest::qExec(&tc, argc, argv);
}
#include "test_thumbloader.moc"
