// MarqueeTabBar: full titles track tab insert/remove/move, and the
// welcome tab invariant (index 0) is observable for the main window's
// bounce-back. Headless-safe with QT_QPA_PLATFORM=offscreen.
#include "tabbar.h"

#include <QApplication>
#include <QTest>

class TestTabBar : public QObject {
    Q_OBJECT
private slots:
    void titlesFollowInserts() {
        MarqueeTabBar bar;
        bar.addTab(QString());
        bar.addTab(QString());
        bar.setFullTitle(0, QStringLiteral("Home"));
        bar.setFullTitle(1, QStringLiteral("Danbooru"));
        QCOMPARE(bar.fullTitle(0), QStringLiteral("Home"));
        QCOMPARE(bar.fullTitle(1), QStringLiteral("Danbooru"));
    }

    void titlesFollowRemove() {
        MarqueeTabBar bar;
        bar.addTab(QString());
        bar.addTab(QString());
        bar.addTab(QString());
        bar.setFullTitle(0, QStringLiteral("Home"));
        bar.setFullTitle(1, QStringLiteral("A"));
        bar.setFullTitle(2, QStringLiteral("B"));
        bar.removeTab(1);
        QCOMPARE(bar.count(), 2);
        QCOMPARE(bar.fullTitle(0), QStringLiteral("Home"));
        QCOMPARE(bar.fullTitle(1), QStringLiteral("B"));
    }

    void titlesFollowMove() {
        MarqueeTabBar bar;
        bar.setMovable(true);
        bar.addTab(QString());
        bar.addTab(QString());
        bar.addTab(QString());
        bar.setFullTitle(0, QStringLiteral("Home"));
        bar.setFullTitle(1, QStringLiteral("A"));
        bar.setFullTitle(2, QStringLiteral("B"));
        bar.moveTab(2, 1);
        QCOMPARE(bar.fullTitle(0), QStringLiteral("Home"));
        QCOMPARE(bar.fullTitle(1), QStringLiteral("B"));
        QCOMPARE(bar.fullTitle(2), QStringLiteral("A"));
    }

    void outOfRangeIsSafe() {
        MarqueeTabBar bar;
        bar.addTab(QStringLiteral("x"));
        QCOMPARE(bar.fullTitle(0), QStringLiteral("x")); // falls back to text
        QCOMPARE(bar.fullTitle(7), QString()); // out of range
        bar.setFullTitle(-1, QStringLiteral("nope"));   // no crash
        bar.removeTab(4);                               // no crash
    }
};

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    TestTabBar tc;
    return QTest::qExec(&tc, argc, argv);
}
#include "test_tabbar.moc"
