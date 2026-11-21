#include "migrations.h"

// janitor-columns: pre-cache-policy tracking DBs lack the media columns
// (nsfw, site). SQLite has no IF NOT EXISTS for columns: probe
// PRAGMA table_info, then add once.
//
// janitorDb() in src/library/cachejanitor.cpp calls
// ensureJanitorColumns() on the live handle, so the per-thread opens keep
// working without the runner.
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>

namespace Migrations {

bool ensureJanitorColumns(QSqlDatabase &db, QString *error) {
    Q_UNUSED(error);
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS files ("
                               "path TEXT PRIMARY KEY, size INTEGER, seen INTEGER,"
                               "nsfw INTEGER DEFAULT -1, site TEXT DEFAULT '')")))
        return false;
    bool hasNsfw = false;
    bool hasSite = false;
    if (QSqlQuery info(db);
        info.exec(QStringLiteral("PRAGMA table_info(files)"))) {
        while (info.next()) {
            const QString colName = info.value(1).toString();
            hasNsfw = hasNsfw || colName == QStringLiteral("nsfw");
            hasSite = hasSite || colName == QStringLiteral("site");
        }
    }
    if (!hasNsfw)
        QSqlQuery(db).exec(
            QStringLiteral("ALTER TABLE files ADD COLUMN nsfw INTEGER DEFAULT -1"));
    if (!hasSite)
        QSqlQuery(db).exec(
            QStringLiteral("ALTER TABLE files ADD COLUMN site TEXT DEFAULT ''"));
    return true;
}

namespace {
bool columnsMissing(QSqlDatabase &db) {
    bool hasNsfw = false;
    bool hasSite = false;
    bool hasTable = false;
    QSqlQuery t(db);
    if (t.exec(QStringLiteral(
            "SELECT 1 FROM sqlite_master WHERE type='table' AND name='files'")) &&
        t.next())
        hasTable = true;
    if (!hasTable)
        return false; // fresh: janitorDb() creates the new schema
    QSqlQuery info(db);
    if (!info.exec(QStringLiteral("PRAGMA table_info(files)")))
        return false;
    while (info.next()) {
        const QString colName = info.value(1).toString();
        hasNsfw = hasNsfw || colName == QStringLiteral("nsfw");
        hasSite = hasSite || colName == QStringLiteral("site");
    }
    return !hasNsfw || !hasSite;
}
} // namespace

bool needsJanitorColumns(const QString &dbPath) {
    if (!QFileInfo::exists(dbPath))
        return false;
    const QString conn = QStringLiteral("mig_janitor_%1").arg(
        QUuid::createUuid().toString(QUuid::WithoutBraces));
    bool need = false;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(
            QStringLiteral("QSQLITE"), conn);
        db.setDatabaseName(dbPath);
        if (db.open()) {
            need = columnsMissing(db);
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(conn);
    return need;
}

bool applyJanitorColumns(const QString &dbPath, QString *error) {
    const QString conn = QStringLiteral("mig_janitor_apply_%1").arg(
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
            ok = ensureJanitorColumns(db, error);
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(conn);
    return ok;
}

} // namespace Migrations
