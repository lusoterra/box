// GifView: animated GIF playback via QMovie on a QGraphicsView.
// Headless-safe with QT_QPA_PLATFORM=offscreen.
#include "viewerpage.h"

#include <QApplication>
#include <QGraphicsScene>
#include <QMovie>
#include <QTest>

class TestGifView : public QObject {
    Q_OBJECT
private slots:
    void animatedGifHasMultipleFrames() {
        QMovie movie(QString::fromLatin1(TESTFILES_DIR) + u"/test_anim.gif");
        QVERIFY(movie.isValid());
        QCOMPARE(movie.frameCount(), 2);
    }

    void singleFrameGifHasOneFrame() {
        QMovie movie(QString::fromLatin1(TESTFILES_DIR) + u"/test_single.gif");
        QVERIFY(movie.isValid());
        QCOMPARE(movie.frameCount(), 1);
    }

    void animatedGifRendersOnScene() {
        GifView view;
        view.setFile(QString::fromLatin1(TESTFILES_DIR) + u"/test_anim.gif");
        // A pixmap item should be on the scene (first frame rendered).
        QCOMPARE(view.scene()->items().size(), 1);
    }

    void singleFrameGifRendersAsStill() {
        GifView view;
        view.setFile(QString::fromLatin1(TESTFILES_DIR) + u"/test_single.gif");
        // Single-frame GIF is shown as a still: one pixmap item, no movie.
        QCOMPARE(view.scene()->items().size(), 1);
    }

    void invalidGifShowsErrorText() {
        GifView view;
        view.setFile(QStringLiteral("/nonexistent/file.gif"));
        // Invalid GIF shows an error text item instead of silently failing.
        QCOMPARE(view.scene()->items().size(), 1);
    }
};

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    TestGifView tc;
    return QTest::qExec(&tc, argc, argv);
}
#include "test_gifview.moc"
