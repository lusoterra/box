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

    void pathEdgeCases() {
        // src/library/stray.h
        // Trailing slash: QFileInfo considers the dir itself, whose parent is _Stray? No.
        // QFileInfo("/dl/_Stray/").dir().dirName() == "_Stray" (parent of dir)
        // Actually, QFileInfo("/dl/_Stray/").dir() is "/dl", dirName "_Stray" -> true
        QVERIFY(Stray::isStrayPath(QStringLiteral("/dl/_Stray/")));
        QVERIFY(!Stray::isStrayPath(QStringLiteral("/dl/_Stray")));
        QVERIFY(Stray::isStrayPath(QStringLiteral("/dl/_Stray/file")));
        QVERIFY(Stray::isStrayPath(QStringLiteral("/dl/_Stray/file.jpg")));
        QVERIFY(Stray::isStrayPath(QStringLiteral("C:/_Stray/x.jpg"))); // QFileInfo treats C: as a dir name, parent is _Stray
        // trailing slash treats path as a directory (parent is x.jpg, not _Stray)
        QVERIFY(!Stray::isStrayPath(QStringLiteral("/dl/_Stray/x.jpg/")));
        QVERIFY(Stray::isStrayPath(QStringLiteral("/dl/_Stray/../_Stray/x.jpg")));
    }

    void strayGaps() {
        // src/library/stray.h
        // isStrayPath empty -> false
        QVERIFY(!Stray::isStrayPath(QString()));
        // isStrayPath with no _Stray folder -> false
        QVERIFY(!Stray::isStrayPath(QStringLiteral("/dl/other/x.jpg")));
        // isStrayPost: remote post never qualifies
        Post p;
        p.isRemote = true;
        p.absFile = QStringLiteral("/dl/_Stray/x.png");
        QVERIFY(!Stray::isStrayPost(p));
        // isStrayPost: local post with empty absFile -> false
        Post p2;
        p2.isRemote = false;
        p2.absFile = QString();
        QVERIFY(!Stray::isStrayPost(p2));
        // isStrayPost: local post in _Stray -> true
        Post p3;
        p3.isRemote = false;
        p3.absFile = QStringLiteral("/dl/_Stray/x.png");
        QVERIFY(Stray::isStrayPost(p3));
    }
};

QTEST_MAIN(TestStray)
#include "test_stray.moc"
