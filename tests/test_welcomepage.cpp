// Welcome-page mascot idle easter egg: after the idle wait the mascot
// leaves its bottom-right corner and bounces inside the page; real
// activity (click/key/resize) snaps it back. Shift+left click starts it
// on demand. Headless (offscreen) widget test.
#include "welcomepage.h"

#include <QApplication>
#include <QGraphicsOpacityEffect>
#include <QImage>
#include <QLabel>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

namespace {
// The mascot is the only label wearing an opacity effect.
QLabel *mascotOf(WelcomePage *page) {
    for (QLabel *label : page->findChildren<QLabel *>()) {
        if (qobject_cast<QGraphicsOpacityEffect *>(label->graphicsEffect()))
            return label;
    }
    return nullptr;
}

QTimer *idleTimerOf(WelcomePage *page) {
    for (QTimer *timer : page->findChildren<QTimer *>()) {
        if (timer->isSingleShot())
            return timer;
    }
    return nullptr;
}

// 100x60 solid PNG on disk; setMascot() takes a path.
QString writeMascot(QTemporaryDir &dir) {
    QImage img(100, 60, QImage::Format_RGB32);
    img.fill(QColor(90, 140, 220));
    const QString path = dir.filePath(QStringLiteral("mascot.png"));
    return img.save(path) ? path : QString();
}
} // namespace

class TestWelcomePage : public QObject {
    Q_OBJECT
private slots:
    void idleStartsBounceAndActivityResets();
    void shiftClickStartsBounce();
    void resizeCountsAsActivity();
};

void TestWelcomePage::idleStartsBounceAndActivityResets() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = writeMascot(dir);
    QVERIFY(!path.isEmpty());

    WelcomePage page;
    page.resize(640, 480);
    page.setMascot(path);
    page.show();
    QTest::qWait(20);

    QLabel *mascot = mascotOf(&page);
    QVERIFY(mascot);
    QTimer *idle = idleTimerOf(&page);
    QVERIFY(idle);
    QCOMPARE(idle->interval(), 180 * 1000); // long idle by default

    const QPoint corner = mascot->pos();
    const QSize chosen = mascot->size();
    idle->start(30);
    QTRY_VERIFY_WITH_TIMEOUT(mascot->pos() != corner, 2000);
    // DVD mode uses the small preset, not the user's chosen size.
    QVERIFY(mascot->height() < chosen.height());

    // A key press anywhere on the home page is activity: snap back to
    // the corner at the chosen size.
    QTest::keyClick(&page, Qt::Key_A);
    QCOMPARE(mascot->pos(), corner);
    QCOMPARE(mascot->size(), chosen);
    // The idle wait starts over after the activity.
    QVERIFY(idle->isActive());
}

void TestWelcomePage::shiftClickStartsBounce() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = writeMascot(dir);
    QVERIFY(!path.isEmpty());

    WelcomePage page;
    page.resize(640, 480);
    page.setMascot(path);
    page.show();
    QTest::qWait(20);

    QLabel *mascot = mascotOf(&page);
    QVERIFY(mascot);
    const QPoint corner = mascot->pos();
    const QSize chosen = mascot->size();

    QTest::mousePress(mascot, Qt::LeftButton, Qt::ShiftModifier, QPoint(8, 8));
    QTRY_VERIFY_WITH_TIMEOUT(mascot->pos() != corner, 2000);
    QVERIFY(mascot->height() < chosen.height());

    // Plain activity stops the easter egg again and restores the size.
    QTest::mousePress(&page, Qt::LeftButton, Qt::NoModifier, QPoint(4, 4));
    QCOMPARE(mascot->pos(), corner);
    QCOMPARE(mascot->size(), chosen);
}

void TestWelcomePage::resizeCountsAsActivity() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = writeMascot(dir);
    QVERIFY(!path.isEmpty());

    WelcomePage page;
    page.resize(640, 480);
    page.setMascot(path);
    page.show();
    QTest::qWait(20);

    QLabel *mascot = mascotOf(&page);
    QVERIFY(mascot);
    const QPoint corner = mascot->pos();
    QTest::mousePress(mascot, Qt::LeftButton, Qt::ShiftModifier, QPoint(8, 8));
    QTRY_VERIFY_WITH_TIMEOUT(mascot->pos() != corner, 2000);

    page.resize(520, 400);
    // After the resize the mascot must be pinned to the new corner, not
    // floating where the bounce left it.
    QVERIFY(mascot->pos() != corner);
    QCOMPARE(mascot->geometry().right() + 1, page.width());
    QCOMPARE(mascot->geometry().bottom() + 1, page.height());
}

QTEST_MAIN(TestWelcomePage)
#include "test_welcomepage.moc"
