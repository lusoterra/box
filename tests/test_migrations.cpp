// Boot migrations: each needs*/apply* pair is idempotent on temp dirs and
// DBs (never the real config/data/cache). The runner marks steps done in
// the ini, so a second run is empty. Fully isolated via config/cache-dir
// overrides + XDG_CACHE_HOME temp dirs.
#include "migrations/migrations.h"
#include "migrations/runner.h"
#include "favdb.h"
#include "settings.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>

namespace {

void writeFile(const QString &path, const QByteArray &data = "x") {
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    QVERIFY(f.write(data) == data.size());
}

bool hasColumn(const QString &dbPath, const QString &table,
               const QString &column) {
    bool found = false;
    const QString conn = QStringLiteral("migtest_%1").arg(
        QUuid::createUuid().toString(QUuid::WithoutBraces));
    {
        QSqlDatabase db =
            QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
        db.setDatabaseName(dbPath);
        if (db.open()) {
            QSqlQuery info(db);
            if (info.exec(QStringLiteral("PRAGMA table_info(%1)").arg(table))) {
                while (info.next()) {
                    if (info.value(1).toString() == column)
                        found = true;
                }
            }
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(conn);
    return found;
}

// Pre-untyped store.db: table without untyped/custom, one NULL row.
void seedLegacyStore(const QString &dbPath) {
    const QString conn = QStringLiteral("migseed_store");
    {
        QSqlDatabase db =
            QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
        db.setDatabaseName(dbPath);
        QVERIFY(db.open());
        QSqlQuery q(db);
        QVERIFY(q.exec(QStringLiteral(
            "CREATE TABLE favourites (site TEXT, post_id INTEGER, rating TEXT, "
            "score INTEGER, ext TEXT, tags TEXT, tags_artist TEXT, "
            "tags_character TEXT, tags_copyright TEXT, tags_general TEXT, "
            "tags_meta TEXT, file_url TEXT, sample_url TEXT, preview_url TEXT, "
            "post_url TEXT, width INTEGER, height INTEGER, added INTEGER, "
            "local_path TEXT, PRIMARY KEY (site, post_id))")));
        QVERIFY(q.exec(QStringLiteral(
            "INSERT INTO favourites (site, post_id, rating, score, ext, tags, "
            "tags_artist, tags_character, tags_copyright, tags_general, "
            "tags_meta, file_url, sample_url, preview_url, post_url, width, "
            "height, added, local_path) VALUES ('Gelbooru', 5, 'general', 0, "
            "'jpg', 'a b', '', '', '', 'a b', '', '', '', '', '', 0, 0, 0, '')")));
        // Legacy (site,0) stray row for the hash-ids migration.
        QVERIFY(q.exec(QStringLiteral(
            "INSERT INTO favourites (site, post_id, rating, score, ext, tags, "
            "tags_artist, tags_character, tags_copyright, tags_general, "
            "tags_meta, file_url, sample_url, preview_url, post_url, width, "
            "height, added, local_path) VALUES ('_Stray', 0, 'general', 0, "
            "'png', 'sky', '', '', '', 'sky', '', "
            "'https://example.com/legacy.png', '', '', '', 0, 0, 1, '')")));
        db.close();
    }
    QSqlDatabase::removeDatabase(conn);
}

int zeroIdRows(const QString &dbPath) {
    int n = -1;
    const QString conn = QStringLiteral("migcount_zero");
    {
        QSqlDatabase db =
            QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
        db.setDatabaseName(dbPath);
        if (db.open()) {
            QSqlQuery q(db);
            if (q.exec(QStringLiteral(
                    "SELECT COUNT(*) FROM favourites WHERE post_id=0")) &&
                q.next())
                n = q.value(0).toInt();
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(conn);
    return n;
}

// Pre-cache-policy tracking DB: files table without nsfw/site.
void seedLegacyJanitor(const QString &dbPath) {
    QDir().mkpath(QFileInfo(dbPath).absolutePath());
    const QString conn = QStringLiteral("migseed_janitor");
    {
        QSqlDatabase db =
            QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
        db.setDatabaseName(dbPath);
        QVERIFY(db.open());
        QSqlQuery q(db);
        QVERIFY(q.exec(QStringLiteral("CREATE TABLE files (path TEXT PRIMARY "
                                      "KEY, size INTEGER, seen INTEGER)")));
        db.close();
    }
    QSqlDatabase::removeDatabase(conn);
}

} // namespace

class TestMigrations : public QObject {
    Q_OBJECT
    QByteArray m_oldCacheHome;
private slots:
    void init() {
        m_oldCacheHome = qgetenv("XDG_CACHE_HOME");
    }
    void cleanup() {
        // Never leak temp roots or overrides into the next case (a
        // failing QVERIFY returns from the slot early).
        if (m_oldCacheHome.isNull())
            qunsetenv("XDG_CACHE_HOME");
        else
            qputenv("XDG_CACHE_HOME", m_oldCacheHome);
        setConfigDirOverride(QString());
        setAppCacheDirOverride(QString());
    }
    void settingsKeysMigrate() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString ini = tmp.filePath(QStringLiteral("s.ini"));
        {
            QSettings c(ini, QSettings::IniFormat);
            c.setValue(QStringLiteral("General/restoreSession"), true);
            c.setValue(QStringLiteral("General/language"), QStringLiteral("fr"));
            c.setValue(QStringLiteral("cache/thumbs"), false);
            c.setValue(QStringLiteral("privacy/paranoia"), true);
            c.setValue(QStringLiteral("search/history"), QStringLiteral("x"));
            c.sync();
        }
        {
            QSettings c(ini, QSettings::IniFormat);
            QVERIFY(Migrations::needsSettingsKeys(c));
            QVERIFY(Migrations::applySettingsKeys(c));
            QCOMPARE(c.value(QStringLiteral("startup/defaultTab")).toString(),
                     QStringLiteral("last"));
            QCOMPARE(c.value(QStringLiteral("app/language")).toString(),
                     QStringLiteral("fr"));
            QCOMPARE(c.value(QStringLiteral("cache/mode")).toString(),
                     QStringLiteral("none"));
            QVERIFY(!c.contains(QStringLiteral("General/restoreSession")));
            QVERIFY(!c.contains(QStringLiteral("privacy/paranoia")));
            QVERIFY(!c.contains(QStringLiteral("search/history")));
            c.sync();
        }
        {
            QSettings c(ini, QSettings::IniFormat);
            QVERIFY(!Migrations::needsSettingsKeys(c)); // idempotent
        }
    }

    void settingsKeysLeaveCurrentAlone() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString ini = tmp.filePath(QStringLiteral("s.ini"));
        {
            QSettings c(ini, QSettings::IniFormat);
            c.setValue(QStringLiteral("startup/defaultTab"),
                       QStringLiteral("welcome"));
            c.setValue(QStringLiteral("cache/mode"), QStringLiteral("all"));
            c.sync();
        }
        QSettings c(ini, QSettings::IniFormat);
        QVERIFY(!Migrations::needsSettingsKeys(c));
    }

    void booruCredsFoldIntoPresets() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        setConfigDirOverride(tmp.path());
        {
            QSettings c(settingsFilePath(), QSettings::IniFormat);
            c.clear();
            c.setValue(QStringLiteral("danbooru/user"),
                       QStringLiteral("duser"));
            c.setValue(QStringLiteral("danbooru/key"),
                       QStringLiteral("dkey"));
            c.setValue(QStringLiteral("shimmie/base"),
                       QStringLiteral("https://shimmie.local"));
            c.sync();
            QVERIFY(Migrations::needsLegacyBooruCredentials(c));
            QString err;
            QVERIFY(Migrations::applyLegacyBooruCredentials(c, &err));
            QVERIFY(!c.contains(QStringLiteral("danbooru/user")));
            QVERIFY(!c.contains(QStringLiteral("danbooru/key")));
            QVERIFY(!c.contains(QStringLiteral("shimmie/base")));
            c.sync();
        }
        // Registry seeded (presets), readable through the normal path.
        {
            QSettings back(settingsFilePath(), QSettings::IniFormat);
            QVERIFY(!loadBoorus(back).isEmpty());
            QVERIFY(!Migrations::needsLegacyBooruCredentials(back));
        }
        setConfigDirOverride(QString());
    }

    void favDbSchemaUpgradesAndBackfills() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString dbPath = tmp.filePath(QStringLiteral("store.db"));
        seedLegacyStore(dbPath);
        QVERIFY(Migrations::needsFavDbSchema(dbPath));
        QString err;
        QVERIFY(Migrations::applyFavDbSchema(dbPath, &err));
        QVERIFY(hasColumn(dbPath, QStringLiteral("favourites"),
                          QStringLiteral("untyped")));
        QVERIFY(hasColumn(dbPath, QStringLiteral("favourites"),
                          QStringLiteral("custom")));
        // Backfill: the pre-untyped row keeps its general tags as untyped.
        const QString conn = QStringLiteral("migcheck_backfill");
        {
            QSqlDatabase db =
                QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
            db.setDatabaseName(dbPath);
            QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY(q.exec(QStringLiteral("SELECT untyped FROM favourites "
                                          "WHERE post_id=5")));
            QVERIFY(q.next());
            QCOMPARE(q.value(0).toString(), QStringLiteral("a b"));
            db.close();
        }
        QSqlDatabase::removeDatabase(conn);
        QVERIFY(!Migrations::needsFavDbSchema(dbPath)); // idempotent
        QVERIFY(!Migrations::needsFavDbSchema(
            tmp.filePath(QStringLiteral("missing.db")))); // fresh: no work
    }

    void favDbHashIdsHealZeroRows() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString dbPath = tmp.filePath(QStringLiteral("store.db"));
        seedLegacyStore(dbPath);
        // Schema first (legacy fixture lacks untyped/custom).
        QVERIFY(Migrations::applyFavDbSchema(dbPath));
        QVERIFY(Migrations::needsFavDbHashIds(dbPath));
        QString err;
        QVERIFY(Migrations::applyFavDbHashIds(dbPath, &err));
        QCOMPARE(zeroIdRows(dbPath), 0);
        // The healed row is reachable (and removable) by stable identity.
        Post probe;
        probe.isRemote = true;
        probe.siteName = QStringLiteral("_Stray");
        probe.fileUrl = QStringLiteral("https://example.com/legacy.png");
        const qint64 healed = FavDb::pidFor(probe);
        QVERIFY(healed != 0);
        const QString conn = QStringLiteral("migcheck_healed");
        {
            QSqlDatabase db =
                QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
            db.setDatabaseName(dbPath);
            QVERIFY(db.open());
            QSqlQuery q(db);
            q.prepare(QStringLiteral("SELECT 1 FROM favourites WHERE site=? "
                                     "AND post_id=?"));
            q.addBindValue(QStringLiteral("_Stray"));
            q.addBindValue(healed);
            QVERIFY(q.exec() && q.next());
            db.close();
        }
        QSqlDatabase::removeDatabase(conn);
    }

    void janitorColumnsUpgrade() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString dbPath = tmp.filePath(QStringLiteral("cache.db"));
        seedLegacyJanitor(dbPath);
        QVERIFY(Migrations::needsJanitorColumns(dbPath));
        QString err;
        QVERIFY(Migrations::applyJanitorColumns(dbPath, &err));
        QVERIFY(hasColumn(dbPath, QStringLiteral("files"),
                          QStringLiteral("nsfw")));
        QVERIFY(hasColumn(dbPath, QStringLiteral("files"),
                          QStringLiteral("site")));
        QVERIFY(!Migrations::needsJanitorColumns(dbPath)); // idempotent
        QVERIFY(!Migrations::needsJanitorColumns(
            tmp.filePath(QStringLiteral("missing.db"))));
    }

    void cacheLayoutMovesOldTree() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        // Redirect the platform cache root for this test only. Note: with
        // no application name set, QStandardPaths appends the executable
        // name - build the old tree from the computed location, like the
        // migration does.
        qputenv("XDG_CACHE_HOME", tmp.filePath("xdg").toUtf8());
        const QString cacheBase = tmp.filePath(QStringLiteral("cache"));
        const QString dataDir = tmp.filePath(QStringLiteral("data"));
        const QString oldTree =
            QStandardPaths::writableLocation(QStandardPaths::CacheLocation) +
            QStringLiteral("/booru-explorer/booru-explorer");
        QVERIFY(oldTree.startsWith(tmp.filePath(QStringLiteral("xdg"))));
        QDir().mkpath(oldTree + QStringLiteral("/thumbs"));
        writeFile(oldTree + QStringLiteral("/thumbs/a.jpg"));
        QDir().mkpath(dataDir);
        writeFile(dataDir + QStringLiteral("/cache.db"));
        QVERIFY(Migrations::needsCacheLayout(cacheBase, dataDir));
        QString err;
        QVERIFY(Migrations::applyCacheLayout(cacheBase, dataDir, &err));
        QVERIFY(QFile::exists(cacheBase + QStringLiteral("/thumbs/a.jpg")));
        QVERIFY(QFile::exists(cacheBase + QStringLiteral("/cache.db")));
        QVERIFY(!QFile::exists(oldTree + QStringLiteral("/thumbs/a.jpg")));
        QVERIFY(!Migrations::needsCacheLayout(cacheBase, dataDir));
        qunsetenv("XDG_CACHE_HOME");
    }

    void faviconAdoptMoves() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString cacheDir = tmp.filePath(QStringLiteral("icons"));
        const QString customDir = tmp.filePath(QStringLiteral("fav"));
        QDir().mkpath(cacheDir);
        writeFile(cacheDir + QStringLiteral("/danbooru.png"),
                  QByteArray("\x89PNG fake"));
        QVERIFY(Migrations::needsFaviconAdopt(cacheDir, customDir));
        QString err;
        QVERIFY(Migrations::applyFaviconAdopt(cacheDir, customDir, &err));
        QVERIFY(QFile::exists(customDir + QStringLiteral("/auto/danbooru.png")));
        QVERIFY(!QFile::exists(cacheDir + QStringLiteral("/danbooru.png")));
        QVERIFY(!Migrations::needsFaviconAdopt(cacheDir, customDir));
        // Empty custom dir is a no-op, never needed.
        QVERIFY(!Migrations::needsFaviconAdopt(cacheDir, QString()));
    }

    void runnerMarksDoneAndSkipsSecondRun() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString cfg = tmp.filePath(QStringLiteral("config"));
        const QString cache = tmp.filePath(QStringLiteral("cache"));
        const QString data = tmp.filePath(QStringLiteral("data"));
        QDir().mkpath(cfg);
        QDir().mkpath(cache);
        QDir().mkpath(data);
        setConfigDirOverride(cfg);
        setAppCacheDirOverride(cache);
        // Redirect the platform cache root too: the cache-layout probe
        // reads the pre-rename tree from the real location otherwise.
        qputenv("XDG_CACHE_HOME", tmp.filePath("xdg").toUtf8());
        // Legacy state: old settings keys + legacy creds + pre-migration DBs.
        {
            QSettings c(settingsFilePath(), QSettings::IniFormat);
            c.setValue(QStringLiteral("General/language"),
                       QStringLiteral("fr"));
            c.setValue(QStringLiteral("danbooru/user"),
                       QStringLiteral("duser"));
            c.setValue(QStringLiteral("danbooru/key"),
                       QStringLiteral("dkey"));
            c.sync();
        }
        seedLegacyStore(data + QStringLiteral("/store.db"));
        seedLegacyJanitor(cache + QStringLiteral("/cache.db"));

        AppSettings s;
        s.dataDir = data;
        QStringList first = MigrationRunner::runPending(nullptr, s);
        QVERIFY(first.contains(QStringLiteral("settings-keys")));
        QVERIFY(first.contains(QStringLiteral("booru-credentials")));
        QVERIFY(first.contains(QStringLiteral("favdb-schema")));
        QVERIFY(first.contains(QStringLiteral("favdb-hash-ids")));
        QVERIFY(first.contains(QStringLiteral("janitor-columns")));
        QCOMPARE(zeroIdRows(data + QStringLiteral("/store.db")), 0);
        QVERIFY(hasColumn(data + QStringLiteral("/store.db"),
                          QStringLiteral("favourites"),
                          QStringLiteral("custom")));

        // Second boot: everything recorded done, no work, no dialog.
        QStringList second = MigrationRunner::runPending(nullptr, s);
        QVERIFY(second.isEmpty());

        // Done-keys + version persisted in the ini.
        {
            QSettings c(settingsFilePath(), QSettings::IniFormat);
            const QStringList done =
                c.value(QStringLiteral("migrations/done")).toStringList();
            QVERIFY(done.contains(QStringLiteral("settings-keys")));
            QVERIFY(done.contains(QStringLiteral("favdb-schema|") + data));
            QCOMPARE(c.value(QStringLiteral("migrations/version")).toInt(),
                     MigrationRunner::kVersion);
        }
        setAppCacheDirOverride(QString());
        setConfigDirOverride(QString());
        qunsetenv("XDG_CACHE_HOME");
    }
};

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    TestMigrations tc;
    return QTest::qExec(&tc, argc, argv);
}
#include "test_migrations.moc"
