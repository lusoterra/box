// Favourites database: add/isFavourite/remove/count/list/clear in a
// temp dir (never the real data dir).
#include "favdb.h"

#include <QFile>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QCoreApplication>
#include <QtTest>

namespace {
Post samplePost() {
    Post p;
    p.isRemote = true;
    p.siteName = QStringLiteral("Gelbooru");
    p.id = 7103104;
    p.rating = QStringLiteral("general");
    p.score = 7;
    p.ext = QStringLiteral("jpg");
    p.tags = QStringLiteral("1girl sky");
    p.tagsGeneral = p.tags;
    p.fileUrl = QStringLiteral("https://img/x.jpg");
    p.sampleUrl = QStringLiteral("https://img/x_s.jpg");
    p.previewUrl = QStringLiteral("https://img/x_p.jpg");
    p.postUrl = QStringLiteral("https://gelbooru.com/index.php?page=post&s=view&id=7103104");
    p.width = 800;
    p.height = 600;
    p.hasMeta = true;
    return p;
}
} // namespace

class TestFavDb : public QObject {
    Q_OBJECT
private slots:
    void addFindRemove() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        FavDb db;
        QVERIFY(db.open(tmp.path()));
        QVERIFY(!db.isFavourite(QStringLiteral("Gelbooru"), 7103104));
        QVERIFY(db.add(samplePost()));
        QVERIFY(!db.add(samplePost())); // duplicate rejected
        QVERIFY(db.isFavourite(QStringLiteral("Gelbooru"), 7103104));
        QCOMPARE(db.count(), 1);
        QVERIFY(db.remove(QStringLiteral("Gelbooru"), 7103104));
        QVERIFY(!db.isFavourite(QStringLiteral("Gelbooru"), 7103104));
        QCOMPARE(db.count(), 0);
    }

    void idlessImportsKeyByUrl() {
        // Stray link posts all carry id 0: they must favourite (and
        // un-favourite) independently by URL hash, not collide on (site, 0).
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        FavDb db;
        QVERIFY(db.open(tmp.path()));
        Post a = samplePost();
        a.siteName = QStringLiteral("_Stray");
        a.id = 0;
        a.fileUrl = QStringLiteral("https://example.com/a.png");
        Post b = samplePost();
        b.siteName = QStringLiteral("_Stray");
        b.id = 0;
        b.fileUrl = QStringLiteral("https://example.com/b.png");
        QVERIFY(FavDb::pidFor(a) != FavDb::pidFor(b));
        QVERIFY(db.add(a));
        QVERIFY(db.add(b));
        QCOMPARE(db.count(), 2);
        QVERIFY(!db.add(a)); // same URL: duplicate rejected
        QVERIFY(db.isFavourite(QStringLiteral("_Stray"), FavDb::pidFor(a)));
        QVERIFY(
            db.remove(QStringLiteral("_Stray"), FavDb::pidFor(a)));
        QCOMPARE(db.count(), 1);
        QVERIFY(db.isFavourite(QStringLiteral("_Stray"), FavDb::pidFor(b)));
    }

    void legacyHashRowsHealOnOpen() {
        // Stuck favourites: rows keyed by unstable identities ((site,0)
        // from before URL-hash identity, qHash rows from before the stable
        // scheme) are rewritten to stable ids on open, so unfavouriting
        // works again. True API ids are untouched.
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        {
            // Pre-seed legacy rows with raw SQL (bypassing add()).
            QSqlDatabase db =
                QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                          QStringLiteral("legacyseed"));
            db.setDatabaseName(
                tmp.filePath(QStringLiteral("store.db")));
            QVERIFY(db.open());
            QSqlQuery setup(db);
            QVERIFY(setup.exec(QStringLiteral(
                "CREATE TABLE favourites (site TEXT, post_id INTEGER, "
                "rating TEXT, score INTEGER, ext TEXT, tags TEXT, "
                "tags_artist TEXT, tags_character TEXT, tags_copyright TEXT, "
                "tags_general TEXT, tags_meta TEXT, file_url TEXT, "
                "sample_url TEXT, preview_url TEXT, post_url TEXT, "
                "width INTEGER, height INTEGER, added INTEGER, "
                "local_path TEXT, untyped TEXT, "
                "PRIMARY KEY (site, post_id))")));
            QSqlQuery q(db);
            // (a) pre-identity (site,0) stray row.
            QVERIFY(q.exec(QStringLiteral(
                "INSERT INTO favourites (site, post_id, rating, score, ext, tags, "
                "tags_artist, tags_character, tags_copyright, tags_general, "
                "tags_meta, file_url, sample_url, preview_url, post_url, width, "
                "height, added, local_path, untyped) VALUES ('_Stray', 0, "
                "'general', 0, 'png', 'sky', '', '', '', 'sky', '', "
                "'https://example.com/legacy.png', '', '', '', 0, 0, 1, "
                "NULL, 'sky')")));
            // (b) normal API-id row: must survive untouched.
            QVERIFY(q.exec(QStringLiteral(
                "INSERT INTO favourites (site, post_id, rating, score, ext, tags, "
                "tags_artist, tags_character, tags_copyright, tags_general, "
                "tags_meta, file_url, sample_url, preview_url, post_url, width, "
                "height, added, local_path, untyped) VALUES ('Danbooru', 4242, "
                "'general', 0, 'jpg', 'sky', '', '', '', 'sky', '', "
                "'https://example.com/x.jpg', '', '', '', 0, 0, 1, NULL, "
                "'sky')")));
            db.close();
        }
        QSqlDatabase::removeDatabase(QStringLiteral("legacyseed"));
        FavDb db;
        QVERIFY(db.open(tmp.path()));
        Post probe;
        probe.isRemote = true;
        probe.siteName = QStringLiteral("_Stray");
        probe.id = 0;
        probe.fileUrl = QStringLiteral("https://example.com/legacy.png");
        const qint64 healed = FavDb::pidFor(probe);
        QVERIFY(healed != 0);
        // The stuck row is now reachable (and removable) by identity.
        QVERIFY(db.isFavourite(QStringLiteral("_Stray"), healed));
        QVERIFY(db.remove(QStringLiteral("_Stray"), healed));
        QCOMPARE(db.count(), 1);
        // Untouched survivor still keyed by API id.
        QVERIFY(db.isFavourite(QStringLiteral("Danbooru"), 4242));
    }

    void localFavKeepsFilePath() {
        // Regression: archive posts carry a source URL in postUrl; the
        // favourites list must still resolve to the file on disk.
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        FavDb db;
        QVERIFY(db.open(tmp.path()));
        Post p;
        p.isRemote = false;
        p.absFile = tmp.filePath(QStringLiteral("pic.jpg"));
        p.file = p.absFile;
        p.postUrl = QStringLiteral("https://gelbooru.com/index.php?page=post&s=view&id=1");
        p.rating = QStringLiteral("general");
        p.tags = QStringLiteral("sky");
        p.tagsGeneral = p.tags;
        p.ext = QStringLiteral("jpg");
        // File must exist for rowToPost to serve it locally.
        QFile f(p.absFile);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("x");
        f.close();
        QVERIFY(db.add(p));
        QCOMPARE(db.count(), 1);
        const QVector<Post> listed = db.list();
        QCOMPARE(listed.size(), 1);
        QVERIFY(!listed.first().isRemote);
        QCOMPARE(listed.first().absFile, p.absFile);
    }

    void addManyBatchesOneTransaction() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        FavDb db;
        QVERIFY(db.open(tmp.path()));
        QVector<Post> posts;
        for (int i = 0; i < 50; ++i) {
            Post p = samplePost();
            p.id = 1000 + i;
            posts.append(p);
        }
        posts.append(samplePost()); // id collides below, not here
        QCOMPARE(db.addMany(posts), 51);
        QCOMPARE(db.count(), 51);
        // Duplicates skipped, still one transaction.
        QCOMPARE(db.addMany(posts), 0);
        QCOMPARE(db.count(), 51);
        QVector<Post> mixed = posts.mid(0, 10);
        Post fresh = samplePost();
        fresh.id = 9999;
        mixed.append(fresh);
        QCOMPARE(db.addMany(mixed), 1);
        QCOMPARE(db.count(), 52);
    }

    void listRoundTripsFields() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        FavDb db;
        QVERIFY(db.open(tmp.path()));
        QVERIFY(db.add(samplePost()));
        const QVector<Post> posts = db.list();
        QCOMPARE(posts.size(), 1);
        const Post &p = posts.first();
        QVERIFY(p.isRemote && p.hasMeta);
        QCOMPARE(p.siteName, QStringLiteral("Gelbooru"));
        QCOMPARE(p.id, 7103104LL);
        QCOMPARE(p.rating, QStringLiteral("general"));
        QCOMPARE(p.score, 7);
        QCOMPARE(p.ext, QStringLiteral("jpg"));
        QCOMPARE(p.tags, QStringLiteral("1girl sky"));
        QCOMPARE(p.fileUrl, QStringLiteral("https://img/x.jpg"));
        QCOMPARE(p.sampleUrl, QStringLiteral("https://img/x_s.jpg"));
        QCOMPARE(p.previewUrl, QStringLiteral("https://img/x_p.jpg"));
        QCOMPARE(p.postUrl,
                 QStringLiteral("https://gelbooru.com/index.php?page=post&s=view&id=7103104"));
        QCOMPARE(p.width, 800);
        QCOMPARE(p.height, 600);
        QCOMPARE(p.category, QStringLiteral("Gelbooru"));
    }

    void customBucketsRoundTrip() {
        // Native instance categories survive the DB (and empty stays empty).
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        FavDb db;
        QVERIFY(db.open(tmp.path()));
        Post p = samplePost();
        p.customTags = {{"meme", {"dank", "lol"}},
                        {"oc", {"my_guy"}}};
        QVERIFY(db.add(p));
        const QVector<Post> posts = db.list();
        QCOMPARE(posts.size(), 1);
        QCOMPARE(posts.first().customTags,
                 (QMap<QString, QStringList>(
                     {{"meme", {"dank", "lol"}}, {"oc", {"my_guy"}}})));
    }

    void tagSplitsRoundTrip() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        FavDb db;
        QVERIFY(db.open(tmp.path()));
        QVERIFY(db.add(samplePost()));
        FavDb::TagSplit split;
        split.artist = QStringLiteral("an_artist");
        split.character = QStringLiteral("some_character");
        split.copyright = QString();
        split.general = QStringLiteral("1girl sky");
        split.meta = QString();
        split.untyped = QStringLiteral("an_artist some_character");
        QHash<QPair<QString, qint64>, FavDb::TagSplit> splits;
        splits.insert({QStringLiteral("Gelbooru"), 7103104LL}, split);
        QCOMPARE(db.setTagSplits(splits), 1);
        const QVector<Post> posts = db.list();
        QCOMPARE(posts.size(), 1);
        QCOMPARE(posts.first().tagsArtist, QStringLiteral("an_artist"));
        QCOMPARE(posts.first().tagsCharacter, QStringLiteral("some_character"));
        QCOMPARE(posts.first().tagsGeneral, QStringLiteral("1girl sky"));
        // Unknown rows are ignored, not created.
        QHash<QPair<QString, qint64>, FavDb::TagSplit> ghost;
        ghost.insert({QStringLiteral("Gelbooru"), 1LL}, split);
        QCOMPARE(db.setTagSplits(ghost), 0);
    }

    void listNewestFirstAndClear() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        FavDb db;
        QVERIFY(db.open(tmp.path()));
        Post a = samplePost();
        a.id = 1;
        Post b = samplePost();
        b.id = 2;
        QVERIFY(db.add(a));
        QVERIFY(db.add(b));
        const QVector<Post> posts = db.list();
        QCOMPARE(posts.size(), 2);
        QCOMPARE(posts.at(0).id, 2LL); // newest first
        QCOMPARE(posts.at(1).id, 1LL);
        db.clear();
        QCOMPARE(db.count(), 0);
        QVERIFY(db.list().isEmpty());
    }

    void localPathResolution() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        FavDb db;
        QVERIFY(db.open(tmp.path()));
        // A real file on disk.
        const QString real = tmp.filePath(QStringLiteral("pic.jpg"));
        QFile f(real);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("jpeg");
        f.close();
        Post p = samplePost();
        QVERIFY(db.add(p));
        // Remote first (no path recorded).
        QCOMPARE(db.list().first().isRemote, true);
        // Nothing recorded -> one unlinked row.
        QCOMPARE(db.unlinked().size(), 1);
        QCOMPARE(db.unlinked().first().id, p.id);
        QVERIFY(db.setLocalPath(QStringLiteral("Gelbooru"), p.id, real));
        QVERIFY(db.unlinked().isEmpty());
        const Post back = db.list().first();
        QVERIFY(!back.isRemote);
        QCOMPARE(back.absFile, real);
        QCOMPARE(back.ext, QStringLiteral("jpg"));
        // Missing file falls back to remote.
        QVERIFY(db.setLocalPath(QStringLiteral("Gelbooru"), p.id,
                                tmp.filePath(QStringLiteral("gone.jpg"))));
        QVERIFY(db.list().first().isRemote);
        // Batch paths: one transaction, per-row counts.
        Post q = samplePost();
        q.id = 555;
        QVERIFY(db.add(q));
        QHash<QPair<QString, qint64>, QString> paths;
        paths.insert({QStringLiteral("Gelbooru"), 555LL}, real);
        paths.insert({QStringLiteral("Gelbooru"), 999LL}, real); // no such row
        QCOMPARE(db.setLocalPaths(paths), 1);
        // 555 resolves locally; the gone.jpg row is reported unlinked
        // again (moved/deleted files come back for relinking).
        QCOMPARE(db.unlinked().size(), 1);
        QCOMPARE(db.unlinked().first().id, p.id);
        // 555 resolves locally; the gone.jpg row falls back to remote.
        for (const Post &b : db.list()) {
            if (b.id == 555) {
                QVERIFY(!b.isRemote);
                QCOMPARE(b.absFile, real);
            } else if (b.id == p.id) {
                QVERIFY(b.isRemote);
            }
        }
    }

    void untypedRoundTrips() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        FavDb db;
        QVERIFY(db.open(tmp.path()));
        Post p = samplePost();
        p.untyped = QStringLiteral("hatsune_miku vocaloid");
        QVERIFY(db.add(p));
        QCOMPARE(db.list().first().untyped, QStringLiteral("hatsune_miku vocaloid"));
    }

    void legacyBackfill() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        {
            // Simulate a pre-untyped store.db: table without the column.
            QSqlDatabase cdb =
                QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("legacy"));
            cdb.setDatabaseName(tmp.filePath(QStringLiteral("store.db")));
            QVERIFY(cdb.open());
            QSqlQuery q(cdb);
            QVERIFY(q.exec(QStringLiteral(
                "CREATE TABLE favourites (site TEXT, post_id INTEGER, rating TEXT, "
                "score INTEGER, ext TEXT, tags TEXT, tags_artist TEXT, tags_character TEXT, "
                "tags_copyright TEXT, tags_general TEXT, tags_meta TEXT, file_url TEXT, "
                "sample_url TEXT, preview_url TEXT, post_url TEXT, width INTEGER, "
                "height INTEGER, added INTEGER, local_path TEXT, "
                "PRIMARY KEY (site, post_id))")));
            QVERIFY(q.exec(QStringLiteral(
                "INSERT INTO favourites (site, post_id, rating, score, ext, tags, "
                "tags_artist, tags_character, tags_copyright, tags_general, "
                "tags_meta, file_url, sample_url, preview_url, post_url, width, "
                "height, added, local_path) VALUES ('Gelbooru', 5, "
                "'general', 0, 'jpg', 'a b', '', '', '', 'a b', '', '', '', '', "
                "'', 0, 0, 0, '')")));
            cdb.close();
        }
        QSqlDatabase::removeDatabase(QStringLiteral("legacy"));
        FavDb db;
        QVERIFY(db.open(tmp.path())); // migrates + backfills
        QCOMPARE(db.list().size(), 1);
        QCOMPARE(db.list().first().untyped, QStringLiteral("a b"));
    }

    void explicitRatingHealsOnList() {
        // LOSN imports store "general" with no rating source: listing
        // upgrades rows whose tags hit the explicit list (same rule the
        // scanner applies) and heals the stored value once.
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        FavDb db;
        QVERIFY(db.open(tmp.path()));
        Post e = samplePost();
        e.id = 11;
        e.tags = QStringLiteral("1girl nude");
        e.tagsGeneral = e.tags;
        QVERIFY(db.add(e));
        Post g = samplePost();
        g.id = 12;
        QVERIFY(db.add(g)); // "1girl sky": stays general
        QVector<Post> listed = db.list();
        QCOMPARE(listed.size(), 2);
        for (const Post &p : listed) {
            if (p.id == 11) {
                QCOMPARE(p.rating, QStringLiteral("explicit"));
                QVERIFY(p.ratingInferred);
            } else {
                QCOMPARE(p.rating, QStringLiteral("general"));
                QVERIFY(!p.ratingInferred);
            }
        }
        // Stored value healed: raw SQL sees explicit, not general.
        {
            QSqlDatabase cdb =
                QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                          QStringLiteral("healcheck"));
            cdb.setDatabaseName(tmp.filePath(QStringLiteral("store.db")));
            QVERIFY(cdb.open());
            QSqlQuery q(cdb);
            QVERIFY(q.exec(QStringLiteral(
                "SELECT rating FROM favourites WHERE post_id=11")));
            QVERIFY(q.next());
            QCOMPARE(q.value(0).toString(), QStringLiteral("explicit"));
            QVERIFY(q.exec(QStringLiteral(
                "SELECT rating FROM favourites WHERE post_id=12")));
            QVERIFY(q.next());
            QCOMPARE(q.value(0).toString(), QStringLiteral("general"));
            cdb.close();
        }
        QSqlDatabase::removeDatabase(QStringLiteral("healcheck"));
    }

    void localPathsMap() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        FavDb db;
        QVERIFY(db.open(tmp.path()));
        Post p = samplePost();
        Post q = samplePost();
        q.id = 555;
        QVERIFY(db.add(p));
        QVERIFY(db.add(q));
        auto paths = db.localPaths();
        QCOMPARE(paths.size(), 2);
        // Nothing linked yet: both rows map to empty.
        QVERIFY(paths.value({QStringLiteral("Gelbooru"), p.id}).isEmpty());
        const QString real = tmp.filePath(QStringLiteral("pic.jpg"));
        QVERIFY(db.setLocalPath(QStringLiteral("Gelbooru"), q.id, real));
        paths = db.localPaths();
        QCOMPARE(paths.value({QStringLiteral("Gelbooru"), q.id}), real);
        QVERIFY(paths.value({QStringLiteral("Gelbooru"), p.id}).isEmpty());
    }

    void postUrlSurvivesLocalLink() {
        // The post page stays the post page after a download links
        // the row to disk (viewer/grid keep pointing at the URL).
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        FavDb db;
        QVERIFY(db.open(tmp.path()));
        const QString real = tmp.filePath(QStringLiteral("pic.jpg"));
        QFile f(real);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("jpeg");
        f.close();
        Post p = samplePost();
        QVERIFY(db.add(p));
        QVERIFY(db.setLocalPath(QStringLiteral("Gelbooru"), p.id, real));
        const Post back = db.list().first();
        QVERIFY(!back.isRemote);
        QCOMPARE(back.absFile, real);
        QCOMPARE(back.postUrl, p.postUrl);
    }

    void strayBackingFileNeverBecomesPostUrl() {
        // Non-http values in the post_url column are backing files
        // (strays), never post links.
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        FavDb db;
        QVERIFY(db.open(tmp.path()));
        Post p = samplePost();
        p.siteName = QStringLiteral("_Stray");
        p.postUrl.clear();
        p.absFile = tmp.filePath(QStringLiteral("stray.jpg"));
        QVERIFY(db.add(p));
        const Post back = db.list().first();
        QVERIFY(back.postUrl.isEmpty());
        QCOMPARE(back.absFile, p.absFile);
    }

    void localPostsUseAbsFile() {        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        FavDb db;
        QVERIFY(db.open(tmp.path()));
        Post p;
        p.absFile = QStringLiteral("/pics/a.jpg");
        p.ext = QStringLiteral("jpg");
        p.tags = QStringLiteral("solo");
        p.tagsGeneral = p.tags;
        p.hasMeta = true;
        QVERIFY(db.add(p)); // site "local", id 0
        const QVector<Post> posts = db.list();
        QCOMPARE(posts.size(), 1);
        QVERIFY(!posts.first().isRemote);
        QCOMPARE(posts.first().absFile, QStringLiteral("/pics/a.jpg"));
        QCOMPARE(posts.first().category, QStringLiteral("Favourites"));
    }
};

int main(int argc, char *argv[]) {
    // QCoreApplication (not appless): QSqlDatabase and friends
    // require an application instance. Still no display needed.
    QCoreApplication app(argc, argv);
    TestFavDb tc;
    return QTest::qExec(&tc, argc, argv);
}
#include "test_favdb.moc"
