// Explicit-tag fallback: defaults, normalization, inference, hash.
// Header-only unit (explicittags.h); no app sources linked.
#include "explicittags.h"

#include <QCoreApplication>
#include <QtTest>

class TestExplicitTags : public QObject {
    Q_OBJECT
private slots:
    void defaultsContainUnambiguousTags() {
        const QSet<QString> &d = defaultExplicitTags();
        QVERIFY(d.contains(QStringLiteral("blowjob")));
        QVERIFY(d.contains(QStringLiteral("penis")));
        QVERIFY(d.contains(QStringLiteral("cum")));
        QVERIFY(d.contains(QStringLiteral("nude")));
        QVERIFY(!d.isEmpty());
    }

    void ambiguousTagsStayOutOfDefaults() {
        // "breasts"/"nipples" appear on SFW posts too: must NOT auto-flag.
        const QSet<QString> &d = defaultExplicitTags();
        QVERIFY(!d.contains(QStringLiteral("breasts")));
        QVERIFY(!d.contains(QStringLiteral("nipples")));
        QVERIFY(!d.contains(QStringLiteral("cleavage")));
    }

    void normalizeRatingMaps() {
        QCOMPARE(normalizeRating(QStringLiteral("g")), QStringLiteral("general"));
        QCOMPARE(normalizeRating(QStringLiteral("S")), QStringLiteral("sensitive"));
        QCOMPARE(normalizeRating(QStringLiteral("q")), QStringLiteral("questionable"));
        QCOMPARE(normalizeRating(QStringLiteral("E")), QStringLiteral("explicit"));
        QCOMPARE(normalizeRating(QStringLiteral("general")), QStringLiteral("general"));
        QCOMPARE(normalizeRating(QStringLiteral("  Sensitive  ")), QStringLiteral("sensitive"));
        QCOMPARE(normalizeRating(QString()), QStringLiteral("general"));
        QCOMPARE(normalizeRating(QStringLiteral("bogus")), QStringLiteral("general"));
        QCOMPARE(normalizeRating(QStringLiteral("?")), QStringLiteral("general"));
    }

    void normalizeExplicitTagsCleans() {
        const QStringList out = normalizeExplicitTags(
            {QStringLiteral(" Foo "), QStringLiteral("foo"), QStringLiteral("BAR"),
             QString(), QStringLiteral("   "), QStringLiteral("bar")});
        QCOMPARE(out, QStringList({QStringLiteral("bar"), QStringLiteral("foo")}));
    }

    void inferExplicitUpgrades() {
        auto r = inferExplicit({QStringLiteral("1girl"), QStringLiteral("blowjob")},
                               QStringLiteral("general"));
        QCOMPARE(r.first, QStringLiteral("explicit"));
        QVERIFY(r.second);
    }

    void inferExplicitIsCaseInsensitive() {
        auto r = inferExplicit({QStringLiteral("BlowJob")}, QStringLiteral("general"));
        QCOMPARE(r.first, QStringLiteral("explicit"));
        QVERIFY(r.second);
    }

    void inferExplicitLeavesCleanPosts() {
        auto r = inferExplicit({QStringLiteral("1girl"), QStringLiteral("breasts")},
                               QStringLiteral("general"));
        QCOMPARE(r.first, QStringLiteral("general"));
        QVERIFY(!r.second);
    }

    void inferExplicitKeepsExplicit() {
        auto r = inferExplicit({QStringLiteral("blowjob")}, QStringLiteral("explicit"));
        QCOMPARE(r.first, QStringLiteral("explicit"));
        QVERIFY(!r.second); // already explicit: not "inferred"
    }

    void inferExplicitKeepsOtherRatings() {
        auto r = inferExplicit({QStringLiteral("school_uniform")},
                               QStringLiteral("sensitive"));
        QCOMPARE(r.first, QStringLiteral("sensitive"));
        QVERIFY(!r.second);
    }

    void activeSetReplaceable() {
        // Defaults active until replaced; restore afterwards so later slots
        // (and other binaries, which share the source default) are unaffected.
        QCOMPARE(activeExplicitTags(), defaultExplicitTags());
        setActiveExplicitTags(QSet<QString>{QStringLiteral("customtag")});
        QCOMPARE(activeExplicitTags(), QSet<QString>{QStringLiteral("customtag")});
        auto r = inferExplicit({QStringLiteral("customtag")}, QStringLiteral("general"));
        QCOMPARE(r.first, QStringLiteral("explicit"));
        auto r2 = inferExplicit({QStringLiteral("blowjob")}, QStringLiteral("general"));
        QCOMPARE(r2.first, QStringLiteral("general")); // defaults swapped out
        setActiveExplicitTags(defaultExplicitTags());
        QCOMPARE(activeExplicitTags(), defaultExplicitTags());
    }

    void hashStableAndSensitive() {
        const QString h1 = explicitTagsHash();
        QCOMPARE(h1.size(), 16);
        QCOMPARE(explicitTagsHash(), h1); // stable across calls
        setActiveExplicitTags(QSet<QString>{QStringLiteral("customtag")});
        QVERIFY(explicitTagsHash() != h1); // edits invalidate scan caches
        setActiveExplicitTags(defaultExplicitTags());
        QCOMPARE(explicitTagsHash(), h1);
    }
};

int main(int argc, char *argv[]) {
    // QCoreApplication (not appless): QSqlDatabase and friends
    // require an application instance. Still no display needed.
    QCoreApplication app(argc, argv);
    TestExplicitTags tc;
    return QTest::qExec(&tc, argc, argv);
}
#include "test_explicittags.moc"
