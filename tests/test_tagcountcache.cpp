// Persistent tag vocabulary cache: prefix queries ranked by post count,
// favourite tags lifted, counts never shrink, rows survive a reopen.
#include "tagcountcache.h"

#include <QCoreApplication>
#include <QTemporaryDir>
#include <QtTest>

namespace {
CachedTag row(const QString &tag, int count,
              const QString &bucket = QStringLiteral("general")) {
    CachedTag t;
    t.tag = tag;
    t.bucket = bucket;
    t.count = count;
    return t;
}
} // namespace

class TestTagCountCache : public QObject {
    Q_OBJECT
private slots:
    void prefixMatchesOnly() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        TagCountCache cache;
        QVERIFY(cache.open(TagCountCache::dbPath(dir.path())));
        cache.upsert({row(QStringLiteral("blue_sky"), 900),
                      row(QStringLiteral("sky"), 500),
                      row(QStringLiteral("bluesky"), 10)});
        const auto out = cache.suggest(QStringLiteral("bl"), 10);
        QCOMPARE(out.size(), 2);
        QCOMPARE(out.at(0).tag, QStringLiteral("blue_sky"));
        QCOMPARE(out.at(1).tag, QStringLiteral("bluesky"));
        // Substring (not a prefix) never shows up.
        QCOMPARE(cache.suggest(QStringLiteral("sky"), 10).size(), 1);
    }

    void ranksExactThenFavThenCount() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        TagCountCache cache;
        QVERIFY(cache.open(TagCountCache::dbPath(dir.path())));
        cache.upsert({row(QStringLiteral("sailor_moon"), 40),
                      row(QStringLiteral("sailor"), 90),
                      row(QStringLiteral("sailor_uniform"), 900)});
        QSet<QString> fav{QStringLiteral("sailor_moon")};
        const auto out = cache.suggest(QStringLiteral("sailor"), 10, fav);
        // Exact tag, then the favourite, then plain matches by count.
        QCOMPARE(out.at(0).tag, QStringLiteral("sailor"));
        QCOMPARE(out.at(1).tag, QStringLiteral("sailor_moon"));
        QCOMPARE(out.at(2).tag, QStringLiteral("sailor_uniform"));
        // Without the favourite, counts decide.
        const auto plain = cache.suggest(QStringLiteral("sailor"), 10);
        QCOMPARE(plain.at(1).tag, QStringLiteral("sailor_uniform"));
        QCOMPARE(plain.at(2).tag, QStringLiteral("sailor_moon"));
    }

    void countsNeverShrink() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        TagCountCache cache;
        QVERIFY(cache.open(TagCountCache::dbPath(dir.path())));
        cache.upsert({row(QStringLiteral("cat"), 500)});
        cache.upsert({row(QStringLiteral("cat"), 0)});
        cache.upsert({row(QStringLiteral("CAT"), 120, QStringLiteral("artist"))});
        const auto out = cache.suggest(QStringLiteral("cat"), 5);
        QCOMPARE(out.size(), 1);
        QCOMPARE(out.at(0).count, 500); // the 0/120 answers did not demote it
        QCOMPARE(out.at(0).tag, QStringLiteral("CAT")); // newest spelling wins
        QCOMPARE(out.at(0).bucket, QStringLiteral("artist"));
    }

    void survivesReopen() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = TagCountCache::dbPath(dir.path());
        {
            TagCountCache first;
            QVERIFY(first.open(path));
            first.upsert({row(QStringLiteral("blonde_hair"), 120000)});
            QCOMPARE(first.size(), 1);
        }
        TagCountCache second;
        QVERIFY(second.open(path)); // migration path (existing file)
        const auto out = second.suggest(QStringLiteral("blo"), 5);
        QCOMPARE(out.size(), 1);
        QCOMPARE(out.at(0).tag, QStringLiteral("blonde_hair"));
        QCOMPARE(out.at(0).count, 120000);
    }

    void emptyInputsAreSafe() {
        TagCountCache closed;
        QCOMPARE(closed.size(), 0);
        QVERIFY(closed.suggest(QStringLiteral("ab"), 5).isEmpty());
        closed.upsert({row(QStringLiteral("ab"), 5)}); // no DB: no crash
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        TagCountCache cache;
        QVERIFY(cache.open(TagCountCache::dbPath(dir.path())));
        QVERIFY(cache.suggest(QString(), 5).isEmpty());
        QVERIFY(cache.suggest(QStringLiteral("ab"), 0).isEmpty());
        cache.upsert({}); // empty batch: no-op
        cache.upsert({row(QString(), 5)}); // blank name dropped
        QCOMPARE(cache.size(), 0);
    }
};

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    TestTagCountCache tc;
    return QTest::qExec(&tc, argc, argv);
}
#include "test_tagcountcache.moc"
