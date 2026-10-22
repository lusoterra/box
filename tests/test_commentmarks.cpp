// Grid comment/note pills: parse-time presence across engine shapes.
// Danbooru/Moebooru expose last-comment/last-note stamps, Gelbooru counts
// + has_notes flags, Szuru commentCount + notes arrays. finishRemote is
// the single funnel, so one test covers every engine's fromX mapper.
#include "booruengine.h"

#include <QCoreApplication>
#include <QtTest>

namespace {
const QString kDanbooru = QStringLiteral("https://danbooru.donmai.us");
const QString kGelbooru = QStringLiteral("https://gelbooru.com");
const QString kKonachan = QStringLiteral("https://konachan.com");
const QString kSzuru = QStringLiteral("https://szuru.example");

QJsonObject danbooruBase() {
    QJsonObject o;
    o[QStringLiteral("id")] = 123;
    o[QStringLiteral("file_url")] =
        QStringLiteral("https://cdn.donmai.us/sample.jpg");
    o[QStringLiteral("file_ext")] = QStringLiteral("jpg");
    o[QStringLiteral("tag_string")] = QStringLiteral("sky");
    return o;
}
} // namespace

class TestCommentMarks : public QObject {
    Q_OBJECT
private slots:
    void danbooruStamps() {
        // No stamps: no pills.
        const Post bare =
            RemotePosts::fromDanbooru(danbooruBase(), kDanbooru);
        QVERIFY(!bare.hasComments);
        QVERIFY(!bare.hasNotes);
        QCOMPARE(bare.commentCount, 0);
        // Comment stamp only.
        QJsonObject c = danbooruBase();
        c[QStringLiteral("last_commented_at")] =
            QStringLiteral("2026-01-01T00:00:00.000-05:00");
        const Post withComments = RemotePosts::fromDanbooru(c, kDanbooru);
        QVERIFY(withComments.hasComments);
        QVERIFY(!withComments.hasNotes);
        // Bumped-at variant counts too.
        QJsonObject b = danbooruBase();
        b[QStringLiteral("last_comment_bumped_at")] =
            QStringLiteral("2026-01-01T00:00:00.000-05:00");
        QVERIFY(RemotePosts::fromDanbooru(b, kDanbooru).hasComments);
        // Note stamp.
        QJsonObject n = danbooruBase();
        n[QStringLiteral("last_noted_at")] =
            QStringLiteral("2026-01-02T00:00:00.000-05:00");
        const Post withNotes = RemotePosts::fromDanbooru(n, kDanbooru);
        QVERIFY(withNotes.hasNotes);
        QVERIFY(!withNotes.hasComments);
    }

    void gelbooruCountsAndFlags() {
        QJsonObject o;
        o[QStringLiteral("id")] = 456;
        o[QStringLiteral("file_url")] =
            QStringLiteral("https://gelbooru.com/img.jpg");
        o[QStringLiteral("tags")] = QStringLiteral("sky");
        QCOMPARE(RemotePosts::fromGelbooru(o, kGelbooru).commentCount, 0);
        o[QStringLiteral("comment_count")] = 7;
        o[QStringLiteral("has_notes")] = QStringLiteral("yes");
        const Post p = RemotePosts::fromGelbooru(o, kGelbooru);
        QCOMPARE(p.commentCount, 7);
        QVERIFY(p.hasComments);
        QVERIFY(p.hasNotes);
        // "no" flag means no notes.
        QJsonObject q = o;
        q[QStringLiteral("has_notes")] = QStringLiteral("no");
        QVERIFY(!RemotePosts::fromGelbooru(q, kGelbooru).hasNotes);
    }

    void moebooruNumericStamps() {
        QJsonObject o;
        o[QStringLiteral("id")] = 789;
        o[QStringLiteral("file_url")] =
            QStringLiteral("https://konachan.com/img.png");
        o[QStringLiteral("file_ext")] = QStringLiteral("png");
        o[QStringLiteral("tags")] = QStringLiteral("sky");
        QVERIFY(!RemotePosts::fromMoebooru(o, kKonachan).hasComments);
        o[QStringLiteral("last_commented_at")] = 1792629076.0;
        o[QStringLiteral("last_noted_at")] = 1792629000.0;
        const Post p = RemotePosts::fromMoebooru(o, kKonachan);
        QVERIFY(p.hasComments);
        QVERIFY(p.hasNotes);
    }

    void szuruCountsAndNotes() {
        QJsonObject o;
        o[QStringLiteral("id")] = 42;
        o[QStringLiteral("safety")] = QStringLiteral("safe");
        o[QStringLiteral("tags")] = QJsonArray();
        o[QStringLiteral("contentUrl")] =
            QStringLiteral("https://szuru.example/data/a.jpg");
        QVERIFY(!RemotePosts::fromSzuru(o, kSzuru).hasComments);
        o[QStringLiteral("commentCount")] = 3;
        QJsonObject note;
        note[QStringLiteral("polygon")] = QJsonArray();
        o[QStringLiteral("notes")] = QJsonArray({note});
        const Post p = RemotePosts::fromSzuru(o, kSzuru);
        QCOMPARE(p.commentCount, 3);
        QVERIFY(p.hasComments);
        QVERIFY(p.hasNotes);
    }
};

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    TestCommentMarks tc;
    return QTest::qExec(&tc, argc, argv);
}
#include "test_commentmarks.moc"
