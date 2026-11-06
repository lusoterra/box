// Cache janitor: size tracking, oldest-seen-first rotation past the cap,
// adoption of untracked files. Temp base + tracking dirs only.
#include "cachejanitor.h"

#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>

namespace {
void writeFile(const QString &path, int bytes) {
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    QVERIFY(f.write(QByteArray(bytes, 'x')) == bytes);
}

int trackedRows(const QString &trackingDir) {
    int n = -1;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                    QStringLiteral("t_janitor_check"));
        db.setDatabaseName(trackingDir + QStringLiteral("/cache.db"));
        if (db.open()) {
            QSqlQuery q(db);
            if (q.exec(QStringLiteral("SELECT COUNT(*) FROM files")) && q.next())
                n = q.value(0).toInt();
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(QStringLiteral("t_janitor_check"));
    return n;
}
} // namespace

class TestCacheJanitor : public QObject {
    Q_OBJECT
private slots:
    void rotatesOldestFirst() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString base = tmp.filePath(QStringLiteral("cache"));
        const QString tracking = tmp.filePath(QStringLiteral("tracking"));
        QDir().mkpath(base + QStringLiteral("/thumbs"));
        CacheJanitor::setTrackingDir(tracking);
        // 90% of 150 = 135: two 100 B files force exactly one eviction.
        CacheJanitor::configure(base, 150);
        const QString a = base + QStringLiteral("/thumbs/a.jpg");
        const QString b = base + QStringLiteral("/thumbs/b.jpg");
        writeFile(a, 100);
        CacheJanitor::record(a);
        QThread::msleep(50); // distinct seen timestamps
        writeFile(b, 100);
        CacheJanitor::record(b);
        CacheJanitor::enforce(true);
        QVERIFY(!QFile::exists(a)); // oldest evicted
        QVERIFY(QFile::exists(b));
    }

    void underCapKeepsEverything() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString base = tmp.filePath(QStringLiteral("cache"));
        const QString tracking = tmp.filePath(QStringLiteral("tracking"));
        QDir().mkpath(base + QStringLiteral("/thumbs"));
        CacheJanitor::setTrackingDir(tracking);
        CacheJanitor::configure(base, 1024 * 1024);
        const QString a = base + QStringLiteral("/thumbs/a.jpg");
        writeFile(a, 100);
        CacheJanitor::record(a);
        CacheJanitor::enforce(true);
        QVERIFY(QFile::exists(a));
        QCOMPARE(trackedRows(tracking), 1);
    }

    void adoptsUntrackedFiles() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString base = tmp.filePath(QStringLiteral("cache"));
        const QString tracking = tmp.filePath(QStringLiteral("tracking"));
        QDir().mkpath(base + QStringLiteral("/thumbs"));
        CacheJanitor::setTrackingDir(tracking);
        CacheJanitor::configure(base, 1024 * 1024);
        writeFile(base + QStringLiteral("/thumbs/ghost.jpg"), 50); // never recorded
        CacheJanitor::enforce(true); // adopt pass
        QCOMPARE(trackedRows(tracking), 1);
        QVERIFY(QFile::exists(base + QStringLiteral("/thumbs/ghost.jpg")));
    }

    void hitRefreshesSeen() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString base = tmp.filePath(QStringLiteral("cache"));
        const QString tracking = tmp.filePath(QStringLiteral("tracking"));
        QDir().mkpath(base + QStringLiteral("/thumbs"));
        CacheJanitor::setTrackingDir(tracking);
        CacheJanitor::configure(base, 150); // target 135
        const QString a = base + QStringLiteral("/thumbs/a.jpg");
        const QString b = base + QStringLiteral("/thumbs/b.jpg");
        writeFile(a, 100);
        writeFile(b, 100);
        CacheJanitor::record(a);
        QThread::msleep(50);
        CacheJanitor::record(b);
        QThread::msleep(50);
        CacheJanitor::hit(a); // a is now newest; b must go
        CacheJanitor::enforce(true);
        QVERIFY(QFile::exists(a));
        QVERIFY(!QFile::exists(b));
    }

    void trackingDirSwitchReopensDb() {
        // Regression: profile switches / data-dir moves must not keep
        // accounting in the old cache.db.
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString base = tmp.filePath(QStringLiteral("cache"));
        QDir().mkpath(base + QStringLiteral("/thumbs"));
        const QString trackA = tmp.filePath(QStringLiteral("trackA"));
        const QString trackB = tmp.filePath(QStringLiteral("trackB"));
        CacheJanitor::setTrackingDir(trackA);
        CacheJanitor::configure(base, 1024 * 1024);
        const QString a = base + QStringLiteral("/thumbs/a.jpg");
        writeFile(a, 10);
        CacheJanitor::record(a);
        QCOMPARE(trackedRows(trackA), 1);
        CacheJanitor::setTrackingDir(trackB); // e.g. profile switch
        const QString b = base + QStringLiteral("/thumbs/b.jpg");
        writeFile(b, 10);
        CacheJanitor::record(b);
        QCOMPARE(trackedRows(trackA), 1); // old DB untouched
        QCOMPARE(trackedRows(trackB), 1); // new DB used
    }

    void purgeNsfwKeepsSfwOnly() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString base = tmp.filePath(QStringLiteral("cache"));
        const QString tracking = tmp.filePath(QStringLiteral("tracking"));
        QDir().mkpath(base + QStringLiteral("/thumbs"));
        QDir().mkpath(base + QStringLiteral("/full"));
        CacheJanitor::setTrackingDir(tracking);
        CacheJanitor::configure(base, 1024 * 1024);
        const QString sfw = base + QStringLiteral("/thumbs/sfw.jpg");
        const QString nsfw = base + QStringLiteral("/thumbs/nsfw.jpg");
        const QString unknown = base + QStringLiteral("/thumbs/legacy.jpg");
        writeFile(sfw, 10);
        writeFile(nsfw, 10);
        writeFile(unknown, 10);
        CacheJanitor::record(sfw, false, QStringLiteral("safe"));
        CacheJanitor::record(nsfw, true, QStringLiteral("gelbooru"));
        CacheJanitor::record(unknown); // unknown must fail closed
        QCOMPARE(CacheJanitor::purgeMedia(/*keepSfwOnly=*/true), 2);
        QVERIFY(QFile::exists(sfw)); // provably SFW survives
        QVERIFY(!QFile::exists(nsfw));
        QVERIFY(!QFile::exists(unknown));
        QCOMPARE(trackedRows(tracking), 1);
    }

    void purgeEverythingLeavesIndexAndDbs() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString base = tmp.filePath(QStringLiteral("cache"));
        const QString tracking = tmp.filePath(QStringLiteral("tracking"));
        QDir().mkpath(base + QStringLiteral("/thumbs"));
        QDir().mkpath(base + QStringLiteral("/full"));
        QDir().mkpath(base + QStringLiteral("/roots"));
        QDir().mkpath(tracking);
        CacheJanitor::setTrackingDir(tracking);
        CacheJanitor::configure(base, 1024 * 1024);
        const QString thumb = base + QStringLiteral("/thumbs/a.jpg");
        const QString full = base + QStringLiteral("/full/b.jpg");
        const QString index = base + QStringLiteral("/roots/index.db");
        writeFile(thumb, 10);
        writeFile(full, 10);
        writeFile(index, 10);
        CacheJanitor::record(thumb, false, QString());
        CacheJanitor::record(full, true, QStringLiteral("x"));
        QCOMPARE(CacheJanitor::purgeMedia(/*keepSfwOnly=*/false), 2);
        QVERIFY(!QFile::exists(thumb));
        QVERIFY(!QFile::exists(full));
        QVERIFY(QFile::exists(index)); // scan index / DBs are not media
        QCOMPARE(trackedRows(tracking), 0);
    }

    void cacheJanitorGaps() {
        // src/library/cachejanitor.h: dbPath
        CacheJanitor::setTrackingDir(QString());
        QVERIFY(!CacheJanitor::dbPath().isEmpty());
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        CacheJanitor::setTrackingDir(tmp.path());
        QCOMPARE(CacheJanitor::dbPath(), tmp.filePath(QStringLiteral("cache.db")));
        CacheJanitor::setTrackingDir(QString());
    }
};

int main(int argc, char *argv[]) {
    // QCoreApplication (not appless): QSqlDatabase and friends
    // require an application instance. Still no display needed.
    QCoreApplication app(argc, argv);
    TestCacheJanitor tc;
    return QTest::qExec(&tc, argc, argv);
}
#include "test_cachejanitor.moc"
