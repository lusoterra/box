// OR-group query parsing/expansion (no network, no widgets).
#include "queryor.h"

#include <QCoreApplication>
#include <QtTest>

class TestQueryOr : public QObject {
    Q_OBJECT
private slots:
    void plainQueryHasNoGroups() {
        const QueryOr::ParsedQuery p =
            QueryOr::parse(QStringLiteral("hatsune_miku solo -sketch"));
        QCOMPARE(p.andTokens,
                 QStringList({QStringLiteral("hatsune_miku"),
                              QStringLiteral("solo"), QStringLiteral("-sketch")}));
        QVERIFY(p.groups.isEmpty());
        bool over = false;
        const QList<QStringList> q = QueryOr::expand(p, QueryOr::kMaxCombos, over);
        QVERIFY(!over);
        QCOMPARE(q.size(), 1);
    }

    void groupSplitsOff() {
        const QueryOr::ParsedQuery p = QueryOr::parse(
            QStringLiteral("solo ~(konata_izumi izumi_konata) -sketch"));
        QCOMPARE(p.andTokens, QStringList({QStringLiteral("solo"),
                                           QStringLiteral("-sketch")}));
        QCOMPARE(p.groups.size(), 1);
        QCOMPARE(p.groups.first(),
                 QStringList({QStringLiteral("konata_izumi"),
                              QStringLiteral("izumi_konata")}));
        bool over = false;
        const QList<QStringList> q = QueryOr::expand(p, QueryOr::kMaxCombos, over);
        QVERIFY(!over);
        QCOMPARE(q.size(), 2);
        QVERIFY(q.at(0).contains(QStringLiteral("konata_izumi")));
        QVERIFY(q.at(1).contains(QStringLiteral("izumi_konata")));
        QVERIFY(q.at(0).contains(QStringLiteral("solo")));
    }

    void loneTildeIsSingleGroup() {
        const QueryOr::ParsedQuery p =
            QueryOr::parse(QStringLiteral("solo ~sketch"));
        QCOMPARE(p.andTokens, QStringList({QStringLiteral("solo")}));
        QCOMPARE(p.groups.size(), 1);
        QCOMPARE(p.groups.first(), QStringList({QStringLiteral("sketch")}));
    }

    void cartesianAndCap() {
        const QueryOr::ParsedQuery p =
            QueryOr::parse(QStringLiteral("~(a b) ~(c d e)"));
        bool over = false;
        const QList<QStringList> q = QueryOr::expand(p, QueryOr::kMaxCombos, over);
        QVERIFY(!over);
        QCOMPARE(q.size(), 6); // 2 x 3
        bool tinyOver = false;
        QueryOr::expand(p, 5, tinyOver);
        QVERIFY(tinyOver);
    }

    void emptyGroupMatchesNothing() {
        const QueryOr::ParsedQuery p =
            QueryOr::parse(QStringLiteral("solo ~()"));
        QCOMPARE(p.groups.size(), 1);
        QVERIFY(p.groups.first().isEmpty());
        bool over = false;
        QVERIFY(QueryOr::expand(p, QueryOr::kMaxCombos, over).isEmpty());
        QVERIFY(!over);
    }

    void unterminatedStaysLiteral() {
        const QueryOr::ParsedQuery p =
            QueryOr::parse(QStringLiteral("solo ~(abc"));
        QVERIFY(p.groups.isEmpty());
        QVERIFY(p.andTokens.contains(QStringLiteral("solo")));
    }

    void groupMatching() {
        const QSet<QString> tags = {QStringLiteral("solo"),
                                    QStringLiteral("konata_izumi")};
        QVERIFY(QueryOr::groupsMatch(
            tags, {QStringList({QStringLiteral("konata_izumi"),
                               QStringLiteral("izumi_konata")})}));
        QVERIFY(!QueryOr::groupsMatch(
            tags, {QStringList({QStringLiteral("izumi_konata")})}));
        // Negation inside groups.
        QVERIFY(QueryOr::groupsMatch(tags, {QStringList({QStringLiteral("-sketch")})}));
        QVERIFY(!QueryOr::groupsMatch(
            tags, {QStringList({QStringLiteral("konata_izumi"),
                               QStringLiteral("-solo")})}));
        // Empty group list always matches (plain AND search).
        QVERIFY(QueryOr::groupsMatch(tags, {}));
    }

    void suggestPrefix() {
        QCOMPARE(QueryOr::suggestBare(QStringLiteral("hatsune")),
                 QStringLiteral("hatsune"));
        QCOMPARE(QueryOr::suggestBare(QStringLiteral("solo miku")),
                 QStringLiteral("miku"));
        QCOMPARE(QueryOr::suggestBare(QStringLiteral("solo -miku")),
                 QStringLiteral("miku"));
        QCOMPARE(QueryOr::suggestBare(QStringLiteral("solo ~(konata")),
                 QStringLiteral("konata"));
        QCOMPARE(QueryOr::suggestBare(QStringLiteral("solo ~miku")),
                 QStringLiteral("miku"));
        QCOMPARE(QueryOr::suggestBare(QStringLiteral("solo ")), QStringLiteral(""));
        QCOMPARE(QueryOr::suggestBare(QStringLiteral("")), QStringLiteral(""));
    }
};

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    TestQueryOr tc;
    return QTest::qExec(&tc, argc, argv);
}
#include "test_queryor.moc"
