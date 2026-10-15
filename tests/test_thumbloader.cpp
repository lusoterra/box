// ThumbLoader: dedicated pool, scroll-spam dedupe (one task per key),
// all exit paths resolve. Headless-safe (QImage only, local files).
#include "thumbloader.h"

#include <QCoreApplication>
#include <QDir>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

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
}

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
};

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    TestThumbLoader tc;
    return QTest::qExec(&tc, argc, argv);
}
#include "test_thumbloader.moc"
