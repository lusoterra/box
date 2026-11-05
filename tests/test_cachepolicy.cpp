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

    void tagsProveSfwAndPost() {
        // src/library/cachepolicy.h:155-197
        Post p;
        p.tags = QStringLiteral("rating:safe sky");
        QVERIFY(MediaCache::tagsProveSfw(p));
        p.tags = QStringLiteral("rating:safe rating:explicit");
        QVERIFY(!MediaCache::tagsProveSfw(p));
        p.tags = QStringLiteral("-rating:safe sky");
        QVERIFY(!MediaCache::tagsProveSfw(p)); // negated token ignored
        p.rating = QStringLiteral("explicit");
        p.tags = QStringLiteral("rating:safe");
        QVERIFY(!MediaCache::isSfwPost(p)); // explicit rating wins
        p.rating.clear();
        p.tags = QStringLiteral("safety:general");
        QVERIFY(MediaCache::isSfwPost(p));
    }

    void persistForExplicitDownload() {
        // src/library/cachepolicy.h:199-214
        BooruSite b;
        b.key = QStringLiteral("s");
        b.cacheMode = CacheMode::Inherit;
        MediaCache::configure(CacheMode::SfwOnly, {b});
        QVERIFY(!MediaCache::persistFor(QStringLiteral("s"), QStringLiteral("explicit")));
        QVERIFY(MediaCache::persistFor(QStringLiteral("s"), QStringLiteral("explicit"), true));
        MediaCache::configure(CacheMode::None, {b});
        QVERIFY(!MediaCache::persistFor(QStringLiteral("s"), QStringLiteral("safe"), true));
        QVERIFY(!MediaCache::persistFor(QStringLiteral("s"), QStringLiteral("explicit"), true));
        MediaCache::configure(CacheMode::All, {});
    }
};

QTEST_MAIN(TestCachePolicy)
#include "test_cachepolicy.moc"
