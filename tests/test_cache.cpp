#include "cache.h"
#include "settings.h"

#include <QDir>
#include <QSet>
#include <QTemporaryDir>
#include <QtTest>

class TestCache : public QObject {
    Q_OBJECT
private slots:
    void scanCacheRoundTrip() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        setAppCacheDirOverride(tmp.path() + QStringLiteral("/cache"));
        const QString root = tmp.path() + QStringLiteral("/root");
        QVERIFY(QDir().mkpath(root));

        ScanCache c;
        QVERIFY(c.open(root, QStringLiteral("hash1")));
        QVERIFY(c.isOpen());

        Post in;
        in.id = 42;
        in.rating = QStringLiteral("safe");
        in.tags = QStringLiteral("a b");
        in.hasMeta = true;
        c.store(QStringLiteral("img.jpg"), 100, 200, in);
        c.finish({QStringLiteral("img.jpg")});

        ScanCache c2;
        QVERIFY(c2.open(root, QStringLiteral("hash1")));
        Post out;
        QVERIFY(c2.lookup(QStringLiteral("img.jpg"), 100, 200, out));
        QCOMPARE(out.id, qint64(42));
        QCOMPARE(out.rating, QStringLiteral("safe"));
        QVERIFY(out.hasMeta);

        // Mtime/size change => miss.
        Post miss;
        QVERIFY(!c2.lookup(QStringLiteral("img.jpg"), 101, 200, miss));
        c2.close();
        c.close();
        setAppCacheDirOverride(QString());
    }

    void finishPrunesUnseen() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        setAppCacheDirOverride(tmp.path() + QStringLiteral("/cache"));
        const QString root = tmp.path() + QStringLiteral("/root");
        QVERIFY(QDir().mkpath(root));

        ScanCache c;
        QVERIFY(c.open(root, QStringLiteral("h")));
        Post p;
        p.id = 1;
        c.store(QStringLiteral("a.jpg"), 1, 1, p);
        p.id = 2;
        c.store(QStringLiteral("b.jpg"), 1, 1, p);
        c.finish({QStringLiteral("a.jpg")}); // b pruned
        c.close();

        ScanCache c2;
        QVERIFY(c2.open(root, QStringLiteral("h")));
        Post out;
        QVERIFY(c2.lookup(QStringLiteral("a.jpg"), 1, 1, out));
        QCOMPARE(out.id, qint64(1));
        QVERIFY(!c2.lookup(QStringLiteral("b.jpg"), 1, 1, out));
        c2.close();
        setAppCacheDirOverride(QString());
    }

    void cacheGaps() {
        // src/library/cache.h: dbPathFor
        const QString root = QStringLiteral("/tmp/opencode/testroot");
        const QString dbPath = ScanCache::dbPathFor(root);
        QVERIFY(dbPath.contains(QStringLiteral("cache")));
        QVERIFY(dbPath.endsWith(QStringLiteral(".db")));
        // dbPathFor with empty root still returns a path
        const QString emptyDb = ScanCache::dbPathFor(QString());
        QVERIFY(!emptyDb.isEmpty());
        // isOpen false before open
        ScanCache cache;
        QVERIFY(!cache.isOpen());
    }
};

QTEST_MAIN(TestCache)
#include "test_cache.moc"
