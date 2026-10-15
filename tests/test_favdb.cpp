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
                "INSERT INTO favourites VALUES ('Gelbooru', 5, 'general', 0, 'jpg', "
                "'a b', '', '', '', 'a b', '', '', '', '', '', 0, 0, 0, '')")));
            cdb.close();
        }
        QSqlDatabase::removeDatabase(QStringLiteral("legacy"));
        FavDb db;
        QVERIFY(db.open(tmp.path())); // migrates + backfills
        QCOMPARE(db.list().size(), 1);
        QCOMPARE(db.list().first().untyped, QStringLiteral("a b"));
    }

    void localPostsUseAbsFile() {
        QTemporaryDir tmp;
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
