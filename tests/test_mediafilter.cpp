// Downloads media-type filter: extension-only mapping.
#include "post.h"

#include <QCoreApplication>
#include <QtTest>

class TestMediaFilter : public QObject {
    Q_OBJECT
private slots:
    void allNeverFilters() {
        QVERIFY(mediaFilterMatchesExt(QStringLiteral("all"), QStringLiteral("mp4")));
        QVERIFY(mediaFilterMatchesExt(QString(), QStringLiteral("mp4")));
    }

    void imagesAreStills() {
        QVERIFY(mediaFilterMatchesExt(QStringLiteral("images"),
                                      QStringLiteral("jpg")));
        QVERIFY(mediaFilterMatchesExt(QStringLiteral("images"),
                                      QStringLiteral("PNG")));
        QVERIFY(!mediaFilterMatchesExt(QStringLiteral("images"),
                                       QStringLiteral("gif")));
        QVERIFY(!mediaFilterMatchesExt(QStringLiteral("images"),
                                       QStringLiteral("webp")));
        QVERIFY(!mediaFilterMatchesExt(QStringLiteral("images"),
                                       QStringLiteral("mp4")));
    }

    void gifsAreAnimatedStills() {
        QVERIFY(mediaFilterMatchesExt(QStringLiteral("gifs"),
                                      QStringLiteral("gif")));
        QVERIFY(mediaFilterMatchesExt(QStringLiteral("gifs"),
                                      QStringLiteral("webp")));
        QVERIFY(mediaFilterMatchesExt(QStringLiteral("gifs"),
                                      QStringLiteral("jxl")));
        QVERIFY(!mediaFilterMatchesExt(QStringLiteral("gifs"),
                                       QStringLiteral("mp4")));
    }

    void animatedAddsVideo() {
        QVERIFY(mediaFilterMatchesExt(QStringLiteral("animated"),
                                      QStringLiteral("gif")));
        QVERIFY(mediaFilterMatchesExt(QStringLiteral("animated"),
                                      QStringLiteral("webp")));
        QVERIFY(mediaFilterMatchesExt(QStringLiteral("animated"),
                                      QStringLiteral("mp4")));
        QVERIFY(mediaFilterMatchesExt(QStringLiteral("animated"),
                                      QStringLiteral("webm")));
        QVERIFY(!mediaFilterMatchesExt(QStringLiteral("animated"),
                                       QStringLiteral("jpg")));
    }

    void videosOnlyVideo() {
        QVERIFY(mediaFilterMatchesExt(QStringLiteral("videos"),
                                      QStringLiteral("mp4")));
        QVERIFY(mediaFilterMatchesExt(QStringLiteral("videos"),
                                      QStringLiteral("webm")));
        QVERIFY(!mediaFilterMatchesExt(QStringLiteral("videos"),
                                       QStringLiteral("gif")));
        QVERIFY(!mediaFilterMatchesExt(QStringLiteral("videos"),
                                       QStringLiteral("jpg")));
    }

    void unknownExtNeverMatchesConcreteFilters() {
        QVERIFY(!mediaFilterMatchesExt(QStringLiteral("images"),
                                       QStringLiteral("zip")));
        QVERIFY(!mediaFilterMatchesExt(QStringLiteral("gifs"),
                                       QStringLiteral("")));
        // Unknown preset stays permissive rather than blanking the grid.
        QVERIFY(mediaFilterMatchesExt(QStringLiteral("nonsense"),
                                      QStringLiteral("zip")));
    }
};

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    TestMediaFilter tc;
    return QTest::qExec(&tc, argc, argv);
}
#include "test_mediafilter.moc"
