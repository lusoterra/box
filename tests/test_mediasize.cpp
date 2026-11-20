// Local media size probe: header-only stills, no network, videos refused.
#include "mediasize.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QTemporaryDir>
#include <QtTest>

class TestMediaSize : public QObject {
    Q_OBJECT
private slots:
    void probesStillDimensions() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString png = dir.path() + QStringLiteral("/a.png");
        QImage img(120, 80, QImage::Format_RGB32);
        img.fill(Qt::red);
        QVERIFY(img.save(png, "PNG"));
        int w = 0, h = 0;
        QVERIFY(MediaSize::probeFile(png, w, h));
        QCOMPARE(w, 120);
        QCOMPARE(h, 80);
        // Legacy (extension-less caller) path: still works.
        QVERIFY(MediaSize::probeStill(png, QStringLiteral("png"), w, h));
    }

    void keepsKnownDimensions() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString png = dir.path() + QStringLiteral("/b.png");
        QVERIFY(QImage(30, 40, QImage::Format_RGB32).save(png, "PNG"));
        int w = 900, h = 700; // already known: never overwritten
        QVERIFY(MediaSize::probeFile(png, w, h));
        QCOMPARE(w, 900);
        QCOMPARE(h, 700);
    }

    void refusesVideosAndJunk() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString fake = dir.path() + QStringLiteral("/clip.mp4");
        QFile f(fake);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("not really a video");
        f.close();
        int w = 0, h = 0;
        QVERIFY(!MediaSize::probeFile(fake, w, h)); // needs a media player
        QVERIFY(!MediaSize::probeFile(dir.path() + QStringLiteral("/nope.jpg"),
                                     w, h));
        QCOMPARE(w, 0);
        QCOMPARE(h, 0);
        QVERIFY(!MediaSize::probeFile(QString(), w, h));
        const QString junk = dir.path() + QStringLiteral("/junk.png");
        QFile j(junk);
        QVERIFY(j.open(QIODevice::WriteOnly));
        j.write("garbage");
        j.close();
        QVERIFY(!MediaSize::probeFile(junk, w, h));
        QCOMPARE(w, 0);
    }
};

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    TestMediaSize tc;
    return QTest::qExec(&tc, argc, argv);
}
#include "test_mediasize.moc"
