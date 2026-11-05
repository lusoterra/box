#include "stray.h"
#include "post.h"

#include <QtTest>

class TestStray : public QObject {
    Q_OBJECT
private slots:
    void conventions() {
        QCOMPARE(Stray::dirName(), QStringLiteral("_Stray"));
        QCOMPARE(Stray::markerKey(), QStringLiteral("stray"));
        QVERIFY(Stray::isStrayPath(QStringLiteral("/downloads/_Stray/_Stray_abc.jpg")));
        QVERIFY(!Stray::isStrayPath(QStringLiteral("/downloads/_Stray/sub/x.jpg")));
        QVERIFY(!Stray::isStrayPath(QStringLiteral("/downloads/other/x.jpg")));
        QVERIFY(!Stray::isStrayPath(QString()));

        Post p;
        p.isRemote = false;
        p.absFile = QStringLiteral("/dl/_Stray/x.png");
        QVERIFY(Stray::isStrayPost(p));
        p.isRemote = true;
        QVERIFY(!Stray::isStrayPost(p));
    }
};

QTEST_MAIN(TestStray)
#include "test_stray.moc"
