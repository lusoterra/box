// Index tag re-bucketing: lowercase untyped collection, case-insensitive
// moves that keep the original spelling, idempotent rescans, manual
// overrides winning over earlier verdicts, General pins.
#include "index.h"

#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

namespace {
Post untypedPost(const QString &general, const QString &untyped) {
    Post p;
    p.tagsGeneral = general;
    p.untyped = untyped;
    p.tags = general;
    p.hasMeta = true;
    return p;
}

QStringList sortedTags(const QString &s) {
    QStringList out = s.split(u' ', Qt::SkipEmptyParts);
    out.sort();
    return out;
}
} // namespace

class TestIndex : public QObject {
    Q_OBJECT
private slots:
    void collectUntypedLowercases() {
        Index idx;
        idx.setPosts({untypedPost(QStringLiteral("Hatsune_Miku solo"),
                                  QStringLiteral("Hatsune_Miku"))});
        // Caches and fetched verdicts are lowercase: the set must match them.
        QCOMPARE(idx.collectUntyped(), QSet<QString>{QStringLiteral("hatsune_miku")});
    }

    void applyMovesKeepingSpelling() {
        Index idx;
        idx.setPosts({untypedPost(QStringLiteral("Hatsune_Miku solo"),
                                  QStringLiteral("Hatsune_Miku"))});
        QHash<QString, QString> verdicts;
        verdicts.insert(QStringLiteral("hatsune_miku"), QStringLiteral("character"));
        QCOMPARE(idx.applyTagTypes(verdicts), 1);
        const Post &p = idx.posts().first();
        QCOMPARE(p.tagsCharacter, QStringLiteral("Hatsune_Miku")); // original case
        QCOMPARE(p.tagsGeneral, QStringLiteral("solo"));
        // Combined set unchanged (sorted comparison).
        QCOMPARE(sortedTags(p.tags), sortedTags(QStringLiteral("Hatsune_Miku solo")));
    }

    void reapplyIsNoop() {
        Index idx;
        idx.setPosts({untypedPost(QStringLiteral("lucky_star solo"),
                                  QStringLiteral("lucky_star"))});
        QHash<QString, QString> verdicts;
        verdicts.insert(QStringLiteral("lucky_star"), QStringLiteral("copyright"));
        QCOMPARE(idx.applyTagTypes(verdicts), 1);
        QCOMPARE(idx.applyTagTypes(verdicts), 0); // already there
        const Post &p = idx.posts().first();
        QCOMPARE(p.tagsCopyright, QStringLiteral("lucky_star"));
        QCOMPARE(p.tagsGeneral, QStringLiteral("solo"));
    }

    void overrideWinsOverEarlierVerdict() {
        // Simulates: fetcher said artist before, user pins copyright now.
        Post p = untypedPost(QStringLiteral("solo"), QStringLiteral("x_tag"));
        p.tagsArtist = QStringLiteral("x_tag");
        Index idx;
        idx.setPosts({p});
        QHash<QString, QString> manual;
        manual.insert(QStringLiteral("x_tag"), QStringLiteral("copyright"));
        QCOMPARE(idx.applyTagTypes(manual), 1);
        const Post &back = idx.posts().first();
        QVERIFY(back.tagsArtist.isEmpty());
        QCOMPARE(back.tagsCopyright, QStringLiteral("x_tag"));
        QCOMPARE(sortedTags(back.tags), sortedTags(QStringLiteral("solo x_tag")));
    }

    void generalPinMovesBack() {
        Post p = untypedPost(QStringLiteral("solo"), QStringLiteral("y_tag"));
        p.tagsArtist = QStringLiteral("y_tag");
        Index idx;
        idx.setPosts({p});
        QHash<QString, QString> manual;
        manual.insert(QStringLiteral("y_tag"), QStringLiteral("general"));
        QCOMPARE(idx.applyTagTypes(manual), 1);
        const Post &back = idx.posts().first();
        QVERIFY(back.tagsArtist.isEmpty());
        QCOMPARE(sortedTags(back.tagsGeneral), sortedTags(QStringLiteral("solo y_tag")));
        QCOMPARE(idx.applyTagTypes(manual), 0);
    }

    void unknownEntriesIgnored() {
        Index idx;
        idx.setPosts({untypedPost(QStringLiteral("solo"), QStringLiteral("solo"))});
        QHash<QString, QString> verdicts;
        verdicts.insert(QStringLiteral("missing"), QStringLiteral("artist"));
        verdicts.insert(QStringLiteral("solo"), QStringLiteral("bogus-bucket"));
        QCOMPARE(idx.applyTagTypes(verdicts), 0);
        QCOMPARE(idx.posts().first().tagsGeneral, QStringLiteral("solo"));
        QCOMPARE(idx.applyTagTypes(QHash<QString, QString>()), 0);
    }

    void statsFollowMoves() {
        Index idx;
        idx.setPosts({untypedPost(QStringLiteral("lucky_star solo"),
                                  QStringLiteral("lucky_star"))});
        QCOMPARE(idx.stats().topGeneral.size(), 2);
        QVERIFY(idx.stats().topCopyrights.isEmpty());
        QHash<QString, QString> verdicts;
        verdicts.insert(QStringLiteral("lucky_star"), QStringLiteral("copyright"));
        idx.applyTagTypes(verdicts);
        const Stats s = idx.stats();
        QCOMPARE(s.topCopyrights.size(), 1);
        QCOMPARE(s.topCopyrights.first().first, QStringLiteral("lucky_star"));
        QCOMPARE(s.topGeneral.size(), 1);
    }

    void ratingShorthandsMatchStoredWords() {
        // rating:g/s/q/e (and safety:) filter the post's rating field;
        // unknown rating:foo keeps match-nothing behavior.
        Post g, e;
        g.tags = g.tagsGeneral = QStringLiteral("sky");
        g.rating = QStringLiteral("general");
        g.hasMeta = true;
        e.tags = e.tagsGeneral = QStringLiteral("sky");
        e.rating = QStringLiteral("explicit");
        e.hasMeta = true;
        Index idx;
        idx.setPosts({g, e});
        QCOMPARE(idx.query({QStringLiteral("rating:g")}, QStringLiteral("all"),
                           QString(), QStringLiteral("id-desc"), 1, 30)
                     .posts.size(),
                 1);
        QCOMPARE(idx.query({QStringLiteral("rating:e"), QStringLiteral("sky")},
                           QStringLiteral("all"), QString(), QStringLiteral("id-desc"),
                           1, 30)
                     .posts.size(),
                 1);
        QVERIFY(idx.query({QStringLiteral("rating:foo")}, QStringLiteral("all"),
                          QString(), QStringLiteral("id-desc"), 1, 30)
                    .posts.isEmpty());
        QVERIFY(idx.query({QStringLiteral("-rating:general")}, QStringLiteral("all"),
                          QString(), QStringLiteral("id-desc"), 1, 30)
                    .posts.size() == 1);
    }

    void addedSortUsesFileMtime() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        // Two files: older content first in scan order, newer mtime second.
        const QString older = tmp.filePath(QStringLiteral("a.jpg"));
        const QString newer = tmp.filePath(QStringLiteral("b.jpg"));
        QFile fa(older);
        QVERIFY(fa.open(QIODevice::WriteOnly));
        fa.write("x");
        fa.close();
        QTest::qWait(1100); // distinct mtimes (1s filesystem granularity)
        QFile fb(newer);
        QVERIFY(fb.open(QIODevice::WriteOnly));
        fb.write("y");
        fb.close();
        Post pa;
        pa.file = older;
        pa.absFile = older;
        pa.ext = QStringLiteral("jpg");
        pa.hasMeta = true;
        Post pb = pa;
        pb.file = newer;
        pb.absFile = newer;
        Index idx;
        idx.setPosts({pa, pb}); // scan order: older first
        const auto res =
            idx.query({}, QStringLiteral("all"), QString(), QStringLiteral("added"), 1, 30);
        QCOMPARE(res.posts.size(), 2);
        QCOMPARE(res.posts.first().absFile, newer); // newest file first
        // Unknown sorts keep id-desc behavior; garbage is safe.
        const auto def =
            idx.query({}, QStringLiteral("all"), QString(), QStringLiteral("bogus"), 1, 30);
        QCOMPARE(def.posts.size(), 2);
    }

    void hygieneDropsGarbage() {
        QVERIFY(!tagTokenOk(QString()));
        QVERIFY(!tagTokenOk(QStringLiteral("animated_gif,")));
        QVERIFY(!tagTokenOk(QStringLiteral("highres)")));
        QVERIFY(!tagTokenOk(QStringLiteral("(oops")));
        QVERIFY(!tagTokenOk(QStringLiteral("a;b")));
        QVERIFY(!tagTokenOk(QStringLiteral("say \"hi\"")));
        QVERIFY(tagTokenOk(QStringLiteral("lucy_(cyberpunk)"))); // legit qualifier
        QVERIFY(tagTokenOk(QStringLiteral("1girl")));
        QCOMPARE(cleanTagTokens({QStringLiteral("ok"), QStringLiteral("bad,"),
                                         QStringLiteral("  spaced  ")}),
                 QStringList({QStringLiteral("ok"), QStringLiteral("spaced")}));
    }

    void bucketOfFindsBuckets() {
        Index idx;
        Post p;
        p.tagsArtist = QStringLiteral("an_artist");
        p.tagsCopyright = QStringLiteral("some_series");
        p.tagsGeneral = QStringLiteral("solo");
        p.tags = QStringLiteral("an_artist some_series solo");
        p.hasMeta = true;
        idx.setPosts({p});
        QCOMPARE(idx.bucketOf(QStringLiteral("an_artist")), QStringLiteral("artist"));
        QCOMPARE(idx.bucketOf(QStringLiteral("some_series")),
                 QStringLiteral("copyright"));
        QCOMPARE(idx.bucketOf(QStringLiteral("solo")), QStringLiteral("general"));
        QCOMPARE(idx.bucketOf(QStringLiteral("nope")), QStringLiteral("general"));
    }

    void suggestExactFirst() {
        // Exact match wins over higher-count prefix siblings: five posts
        // carry skyline, one carries sky - typing sky still offers sky.
        Index idx;
        QVector<Post> posts;
        for (int i = 0; i < 5; ++i) {
            Post p;
            p.tagsGeneral = QStringLiteral("skyline");
            p.hasMeta = true;
            posts.append(p);
        }
        Post one;
        one.tagsGeneral = QStringLiteral("sky");
        one.hasMeta = true;
        posts.append(one);
        idx.setPosts(posts);
        const auto sug = idx.suggest(QStringLiteral("sky"), 12);
        QVERIFY(sug.size() >= 2);
        QCOMPARE(sug.first().tag, QStringLiteral("sky"));
    }

    void suggestFavFirst() {
        // Starred tags outrank higher-count plain prefix matches: five
        // posts carry skyline, one carries skirt, skirt is starred.
        Index idx;
        QVector<Post> posts;
        for (int i = 0; i < 5; ++i) {
            Post p;
            p.tagsGeneral = QStringLiteral("skyline");
            p.hasMeta = true;
            posts.append(p);
        }
        Post one;
        one.tagsGeneral = QStringLiteral("skirt");
        one.hasMeta = true;
        posts.append(one);
        idx.setPosts(posts);
        const auto plain = idx.suggest(QStringLiteral("sk"), 12);
        QCOMPARE(plain.first().tag, QStringLiteral("skyline"));
        const auto starred =
            idx.suggest(QStringLiteral("sk"), 12, {QStringLiteral("skirt")});
        QVERIFY(starred.size() >= 2);
        QCOMPARE(starred.first().tag, QStringLiteral("skirt"));
    }

    void filetypeTokensFilterByExt() {
        // filetype: tokens match post extensions (comma = OR), including
        // the negated stills form; plain tags are unaffected.
        QVERIFY(isFiletypeToken(QStringLiteral("filetype:gif,webp")));
        QVERIFY(isFiletypeToken(QStringLiteral("-filetype:mp4")));
        QVERIFY(!isFiletypeToken(QStringLiteral("gif")));
        Index idx;
        Post gif;
        gif.tags = gif.tagsGeneral = QStringLiteral("1girl");
        gif.ext = QStringLiteral("gif");
        gif.hasMeta = true;
        Post movie;
        movie.tags = movie.tagsGeneral = QStringLiteral("1girl");
        movie.ext = QStringLiteral("mp4");
        movie.hasMeta = true;
        Post still;
        still.tags = still.tagsGeneral = QStringLiteral("1girl");
        still.ext = QStringLiteral("jpg");
        still.hasMeta = true;
        idx.setPosts({gif, movie, still});
        QCOMPARE(idx.query({QStringLiteral("1girl"),
                            QStringLiteral("filetype:gif")},
                           QStringLiteral("all"), {}, QStringLiteral("id-desc"),
                           1, 100)
                     .posts.size(),
                 1);
        QCOMPARE(idx.query({QStringLiteral("1girl"),
                            QStringLiteral("-filetype:gif,mp4")},
                           QStringLiteral("all"), {}, QStringLiteral("id-desc"),
                           1, 100)
                     .posts.size(),
                 1);
        QCOMPARE(idx.query({QStringLiteral("1girl")}, QStringLiteral("all"),
                           {}, QStringLiteral("id-desc"), 1, 100)
                     .posts.size(),
                 3);
    }

    void suggestCarriesBuckets() {
        Index idx;
        Post a;
        a.tagsGeneral = QStringLiteral("lucky_star solo");
        a.untyped = QStringLiteral("lucky_star");
        a.hasMeta = true;
        Post b;
        b.tagsArtist = QStringLiteral("some_artist");
        b.tags = QStringLiteral("some_artist");
        b.hasMeta = true;
        idx.setPosts({a, b});
        const auto sug = idx.suggest(QStringLiteral("lu"), 12);
        QVERIFY(!sug.isEmpty());
        QCOMPARE(sug.first().tag, QStringLiteral("lucky_star"));
        QCOMPARE(sug.first().bucket, QStringLiteral("general"));
        const auto art = idx.suggest(QStringLiteral("some_a"), 12);
        QVERIFY(!art.isEmpty());
        QCOMPARE(art.first().bucket, QStringLiteral("artist"));
        // Short prefixes and empty results stay cheap and safe.
        QVERIFY(idx.suggest(QStringLiteral("x"), 12).isEmpty());
        QVERIFY(idx.suggest(QStringLiteral("zzz-no-match"), 12).isEmpty());
        QVERIFY(idx.suggest(QStringLiteral("lu"), 0).isEmpty());
    }

    void indexHelpersGaps() {
        // src/library/index.cpp:299,310
        Index idx;
        Post p;
        p.untyped = QStringLiteral("Untyped_Tag ANOTHER");
        p.tags = p.untyped;
        idx.setPosts({p});
        const QSet<QString> untyped = idx.collectUntyped();
        QVERIFY(untyped.contains(QStringLiteral("untyped_tag")));
        QVERIFY(untyped.contains(QStringLiteral("another")));
        QCOMPARE(untyped.size(), 2);

        Post p2;
        p2.untyped = QString();
        idx.setPosts({p2});
        QVERIFY(idx.collectUntyped().isEmpty());

        // bucketOf unknown -> general
        QCOMPARE(idx.bucketOf(QStringLiteral("missing")), QStringLiteral("general"));
    }
};

int main(int argc, char *argv[]) {
    // QCoreApplication (not appless): index paths may touch Qt subsystems
    // that require an application instance. Still no display needed.
    QCoreApplication app(argc, argv);
    TestIndex tc;
    return QTest::qExec(&tc, argc, argv);
}
#include "test_index.moc"
