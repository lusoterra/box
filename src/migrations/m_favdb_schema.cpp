#include "migrations.h"

// favdb-schema: store.db grows local_path / untyped / custom columns over
// time; rows predating `untyped` get their general tags backfilled (never
// classified rows keep the whole blob, classified rows already moved the
// resolved tags out, leaving exactly the remainder).
//
// FavDb::open() calls ensureFavDbSchema() on the live handle, so temp-DB
// unit tests (which never run the runner) get the same schema.
#include <QDir>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>

namespace Migrations {

namespace {
bool hasColumn(QSqlDatabase &db, const QString &column) {
    QSqlQuery info(db);
    if (!info.exec(QStringLiteral("PRAGMA table_info(favourites)")))
        return false;
    while (info.next()) {
        if (info.value(1).toString() == column)
            return true;
    }
    return false;
}
} // namespace

bool ensureFavDbSchema(QSqlDatabase &db, QString *error) {
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS favourites ("
                               "site TEXT, post_id INTEGER, rating TEXT, score INTEGER, "
                               "ext TEXT, tags TEXT, tags_artist TEXT, tags_character TEXT, "
                               "tags_copyright TEXT, tags_general TEXT, tags_meta TEXT, "
                               "file_url TEXT, sample_url TEXT, preview_url TEXT, "
                               "post_url TEXT, width INTEGER, height INTEGER, "
                               "added INTEGER, local_path TEXT, untyped TEXT, "
                               "custom TEXT, PRIMARY KEY (site, post_id))"))) {
        if (error)
            *error = q.lastError().databaseText();
        return false;
    }
    QSqlQuery alter(db);
    alter.exec(QStringLiteral(
        "ALTER TABLE favourites ADD COLUMN local_path TEXT"));
    alter.exec(QStringLiteral(
        "ALTER TABLE favourites ADD COLUMN untyped TEXT"));
    alter.exec(QStringLiteral(
        "ALTER TABLE favourites ADD COLUMN custom TEXT"));
    // Fails when the columns already exist: fine.
    QSqlQuery backfill(db);
    backfill.exec(QStringLiteral(
        "UPDATE favourites SET untyped=tags_general WHERE untyped IS NULL"));
    return true;
}

bool needsFavDbSchema(const QString &dbPath) {
    if (!QFileInfo::exists(dbPath))
        return false; // fresh profile: open() creates the new schema
    const QString conn = QStringLiteral("mig_schema_%1").arg(
        QUuid::createUuid().toString(QUuid::WithoutBraces));
    bool need = false;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(
            QStringLiteral("QSQLITE"), conn);
        db.setDatabaseName(dbPath);
        if (db.open()) {
            need = !hasColumn(db, QStringLiteral("local_path")) ||
                   !hasColumn(db, QStringLiteral("untyped")) ||
                   !hasColumn(db, QStringLiteral("custom"));
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(conn);
    return need;
}

bool applyFavDbSchema(const QString &dbPath, QString *error) {
    QDir().mkpath(QFileInfo(dbPath).absolutePath());
    const QString conn = QStringLiteral("mig_schema_apply_%1").arg(
        QUuid::createUuid().toString(QUuid::WithoutBraces));
    bool ok = false;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(
            QStringLiteral("QSQLITE"), conn);
        db.setDatabaseName(dbPath);
        if (!db.open()) {
            if (error)
                *error = db.lastError().databaseText();
        } else {
            ok = ensureFavDbSchema(db, error);
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(conn);
    return ok;
}

} // namespace Migrations
