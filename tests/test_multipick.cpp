// Boorus menu multi-pick: checkable entries toggle in place and keep the
// popup open, so picking several sources does not need a reopen per click.
// Plain entries still behave like a normal menu (trigger + close).
// Headless-safe with QT_QPA_PLATFORM=offscreen.
#include "multipickmenu.h"

#include <QAction>
#include <QApplication>
#include <QSignalSpy>
#include <QTest>

class TestMultiPick : public QObject {
    Q_OBJECT
private slots:
    void checkableEntriesStayOpen() {
        MultiPickMenu menu;
        QAction *gel = menu.addAction(QStringLiteral("Gelbooru"));
        gel->setCheckable(true);
        QAction *dan = menu.addAction(QStringLiteral("Danbooru"));
        dan->setCheckable(true);
        QSignalSpy triggered(&menu, &QMenu::triggered);
        menu.popup(QPoint(120, 120));
        QTRY_VERIFY(menu.isVisible());

        QTest::mouseClick(&menu, Qt::LeftButton, Qt::NoModifier,
                          menu.actionGeometry(gel).center());
        QVERIFY2(menu.isVisible(), "clicking a checkable entry closed the menu");
        QVERIFY(gel->isChecked());

        // The second pick works without reopening the popup.
        QTest::mouseClick(&menu, Qt::LeftButton, Qt::NoModifier,
                          menu.actionGeometry(dan).center());
        QVERIFY(menu.isVisible());
        QVERIFY(dan->isChecked());
        QVERIFY(gel->isChecked());
        QCOMPARE(triggered.count(), 2);

        // A second click on the same entry unchecks it, menu still open.
        QTest::mouseClick(&menu, Qt::LeftButton, Qt::NoModifier,
                          menu.actionGeometry(gel).center());
        QVERIFY(menu.isVisible());
        QVERIFY(!gel->isChecked());
        QVERIFY(dan->isChecked());
        QCOMPARE(triggered.count(), 3);
        menu.hide();
    }

    void plainEntriesStillClose() {
        MultiPickMenu menu;
        QAction *open = menu.addAction(QStringLiteral("Open"));
        menu.popup(QPoint(120, 120));
        QTRY_VERIFY(menu.isVisible());
        QTest::mouseClick(&menu, Qt::LeftButton, Qt::NoModifier,
                          menu.actionGeometry(open).center());
        QTRY_VERIFY(!menu.isVisible());
    }
};

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    TestMultiPick tc;
    return QTest::qExec(&tc, argc, argv);
}
#include "test_multipick.moc"
