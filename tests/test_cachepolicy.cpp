#include "cachepolicy.h"
#include "boorutypes.h"
#include "post.h"

#include <QtTest>

class TestCachePolicy : public QObject {
    Q_OBJECT
private slots:
    void modeKeys() {
        using enum CacheMode;
        QCOMPARE(MediaCache::modeKey(All), QStringLiteral("all"));
        QCOMPARE(MediaCache::modeKey(SfwOnly), QStringLiteral("sfw"));
        QCOMPARE(MediaCache::modeKey(None), QStringLiteral("none"));
        QCOMPARE(MediaCache::modeKey(Inherit), QStringLiteral("inherit"));
        QCOMPARE(MediaCache::modeFromKey(QStringLiteral("sfw"), All), SfwOnly);
        QCOMPARE(MediaCache::modeFromKey(QStringLiteral("sfw-only"), All), SfwOnly);
        QCOMPARE(MediaCache::modeFromKey(QStringLiteral("nothing"), All), None);
        QCOMPARE(MediaCache::modeFromKey(QStringLiteral("bogus"), All), All);
    }

    void stricterMode() {
        using enum CacheMode;
        QCOMPARE(MediaCache::stricter(All, SfwOnly), SfwOnly);
        QCOMPARE(MediaCache::stricter(SfwOnly, All), SfwOnly);
        QCOMPARE(MediaCache::stricter(Inherit, None), None);
        QCOMPARE(MediaCache::stricter(All, Inherit), All);
        QCOMPARE(MediaCache::stricter(None, SfwOnly), None);
    }

    void effectiveModeNarrows() {
        BooruSite b;
        b.key = QStringLiteral("site1");
        b.cacheMode = CacheMode::SfwOnly;
        MediaCache::configure(CacheMode::All, {b});
        QCOMPARE(MediaCache::effectiveMode(QStringLiteral("site1")), CacheMode::SfwOnly);
        QCOMPARE(MediaCache::effectiveMode(QStringLiteral("other")), CacheMode::All);
        MediaCache::configure(CacheMode::None, {b});
        QCOMPARE(MediaCache::effectiveMode(QStringLiteral("site1")), CacheMode::None);
        MediaCache::configure(CacheMode::All, {});
    }

    void ratingBuckets() {
        QVERIFY(MediaCache::isSfwRating(QStringLiteral("safe")));
        QVERIFY(MediaCache::isSfwRating(QStringLiteral("general")));
        QVERIFY(MediaCache::isSfwRating(QStringLiteral("s")));
        QVERIFY(MediaCache::isSfwRating(QStringLiteral("g")));
        QVERIFY(MediaCache::isSfwRating(QStringLiteral(" SAFE ")));
        QVERIFY(!MediaCache::isSfwRating(QStringLiteral("explicit")));
        QVERIFY(!MediaCache::isSfwRating(QStringLiteral("questionable")));
        QVERIFY(!MediaCache::isSfwRating(QStringLiteral("sensitive")));
        QVERIFY(!MediaCache::isSfwRating(QString()));
        QVERIFY(MediaCache::isNsfwRating(QString()));
    }

    void sfwBlacklist() {
        MediaCache::setSfwBlacklist({QStringLiteral(" Gore "), QStringLiteral("gore"),
                                     QStringLiteral(""), QStringLiteral("blood")});
        Post p;
        p.tagsGeneral = QStringLiteral("sky gore tree");
        QVERIFY(MediaCache::postHasBlacklistedTag(p));
        Post p2;
        p2.tagsArtist = QStringLiteral("someone");
        p2.customTags[QStringLiteral("meta")] = {QStringLiteral("Blood")};
        QVERIFY(MediaCache::postHasBlacklistedTag(p2));
        Post clean;
        clean.tagsGeneral = QStringLiteral("sky tree");
        QVERIFY(!MediaCache::postHasBlacklistedTag(clean));
        MediaCache::setSfwBlacklist({});
    }
};

QTEST_MAIN(TestCachePolicy)
#include "test_cachepolicy.moc"
