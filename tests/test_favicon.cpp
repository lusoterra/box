// Favicon candidate URLs: base path first, host root fallback (unit-only,
// no network - QImage/QIcon stay out so QCoreApplication suffices).
#include "faviconcache.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

class TestFavicon : public QObject {
    Q_OBJECT
private slots:
    void plainHost() {
        QCOMPARE(faviconCandidates(QStringLiteral("https://danbooru.donmai.us")),
                 QStringList({QStringLiteral("https://danbooru.donmai.us/favicon.ico")}));
    }

    void trailingSlashTrimmed() {
        QCOMPARE(faviconCandidates(QStringLiteral("https://x.example/booru/")),
                 QStringList({QStringLiteral("https://x.example/booru/favicon.ico"),
                              QStringLiteral("https://x.example/favicon.ico")}));
    }

    void subpathFallsBackToRoot() {
        // Subpath instances (no subdomain) serve the icon at either level.
        QCOMPARE(faviconCandidates(QStringLiteral("https://www.visuabusters.com/booru")),
                 QStringList({QStringLiteral("https://www.visuabusters.com/booru/favicon.ico"),
                              QStringLiteral("https://www.visuabusters.com/favicon.ico")}));
    }

    void emptyBase() {
        QVERIFY(faviconCandidates(QString()).isEmpty());
        QVERIFY(faviconCandidates(QStringLiteral("   ")).isEmpty());
    }

    void customPathFor() {
        // Override lookup is pure path logic (no QPixmap, stays headless).
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        FaviconCache cache(dir.filePath(QStringLiteral("cache")));
        QVERIFY(cache.customPathFor(QStringLiteral("danbooru")).isEmpty());
        cache.setCustomDir(dir.filePath(QStringLiteral("custom")));
        QCOMPARE(cache.customPathFor(QString()),
                 QString());
        QCOMPARE(cache.customPathFor(QStringLiteral("a/b")),
                 QString());
        QCOMPARE(cache.customPathFor(QStringLiteral("danbooru")),
                 dir.filePath(QStringLiteral("custom/danbooru.png")));
        QCOMPARE(cache.autoPathFor(QStringLiteral("danbooru")),
                 dir.filePath(QStringLiteral("custom/auto/danbooru.png")));
    }

    void autoMigration() {
        // Legacy cache-dir icons move into the backup folder on first
        // setCustomDir (bytes only: headless-safe, no image decode).
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QDir().mkpath(dir.filePath(QStringLiteral("cache")));
        QFile seed(dir.filePath(QStringLiteral("cache/danbooru.png")));
        QVERIFY(seed.open(QIODevice::WriteOnly));
        seed.write("png-bytes");
        seed.close();
        FaviconCache cache(dir.filePath(QStringLiteral("cache")));
        cache.setCustomDir(dir.filePath(QStringLiteral("fav")));
        QVERIFY(!QFile::exists(dir.filePath(QStringLiteral("cache/danbooru.png"))));
        QVERIFY(QFile::exists(dir.filePath(QStringLiteral("fav/auto/danbooru.png"))));
        // Second call is a no-op (idempotent across data-dir switches).
        cache.setCustomDir(dir.filePath(QStringLiteral("fav")));
        QVERIFY(QFile::exists(dir.filePath(QStringLiteral("fav/auto/danbooru.png"))));
    }
};

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    TestFavicon tc;
    return QTest::qExec(&tc, argc, argv);
}
#include "test_favicon.moc"
