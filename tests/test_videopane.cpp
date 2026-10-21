// VideoPane: QMediaPlayer setup and playback for mp4/webm.
// Headless-safe with QT_QPA_PLATFORM=offscreen (playback may be a no-op
// without a multimedia backend, but construction and setSource must not crash).
#include "viewerpage.h"

#include <QApplication>
#include <QUrl>
#include <QTest>

class TestVideoPane : public QObject {
    Q_OBJECT
private slots:
    void constructionDoesNotCrash() {
        VideoPane pane;
        QVERIFY(&pane != nullptr);
    }

    void setFileDoesNotCrash() {
        VideoPane pane;
        pane.setFile(QString::fromLatin1(TESTFILES_DIR) + u"/test_video.mp4");
        // Should not crash even if playback is unavailable headless.
    }

    void setLocalUrlDoesNotCrash() {
        VideoPane pane;
        pane.setUrl(QUrl::fromLocalFile(
            QString::fromLatin1(TESTFILES_DIR) + u"/test_video.mp4"));
    }

    void invalidUrlDoesNotCrash() {
        VideoPane pane;
        pane.setUrl(QUrl(QStringLiteral("not-a-valid-url")));
        // Error should be surfaced via errorOccurred, not a crash.
    }

    void invalidFrameDoesNotCrash() {
        // The broken-hardware-surface path: an invalid frame must clear
        // the item (never attempt a ReadOnly map on it).
        VideoFrameItem item;
        item.setFrame(QVideoFrame());
        QVERIFY(item.boundingRect().isEmpty());
    }

    void emptyUrlDoesNotCrash() {
        VideoPane pane;
        pane.setUrl(QUrl());
    }

    void setRateDoesNotCrash() {
        VideoPane pane;
        pane.setRate(1.5);
        pane.setRate(0.1); // clamps to 0.25
        pane.setRate(9.0); // clamps to 2.0
        pane.setFile(QString::fromLatin1(TESTFILES_DIR) + u"/test_video.mp4");
    }
};

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    TestVideoPane tc;
    return QTest::qExec(&tc, argc, argv);
}
#include "test_videopane.moc"
