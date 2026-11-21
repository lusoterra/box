#include "migrations.h"

// favdb-hash-ids: one-time healing of hash-based identities. Rows keyed
// by unstable identities ((site,0) predating URL-hash identity, qHash
// rows predating the SHA1 scheme) get stable ids; true duplicates created
// by the rewrite collapse to one row. True API ids never change.
//
// needsFavDbHashIds() probes for (site,0) rows - the only legacy shape a
// current writer can still encounter (pidFor() keys id-less rows by URL
// hash at insert, so post_id=0 can only come from a pre-migration DB).
// The runner performs the full pass once at boot; FavDb::open() keeps the
// same cheap probe as a safety net for data dirs the runner never saw.
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>

#include <cstring>
#include <functional>

namespace Migrations {

namespace {
// Stable 63-bit id; mirrors stableId()/pidFor() in src/library/favdb.cpp
// (kept local so test targets linking the migrations do not need
// favdb.cpp; test_migrations checks healing against FavDb::pidFor, so
// any divergence fails the suite).
qint64 stableIdFor(const QString &s) {
    const QByteArray h =
        QCryptographicHash::hash(s.toUtf8(), QCryptographicHash::Sha1);
    qint64 id = 0;
    memcpy(&id, h.constData(), sizeof(id));
    return id & 0x7fffffffffffffffLL;
}

qint64 wantedPid(bool isRemote, const QString &site, qint64 pid,
                 const QString &fileUrl, const QString &absFile) {
    if (site.isEmpty() && !isRemote)
        return stableIdFor(absFile);
    if (pid != 0)
        return pid;
    return stableIdFor(fileUrl.isEmpty() ? absFile : fileUrl);
}
} // namespace

bool healFavDbHashIds(QSqlDatabase &db, QString *error) {
    QSqlQuery sel(db);
    if (!sel.exec(QStringLiteral(
            "SELECT site, post_id, file_url, post_url FROM favourites"))) {
        if (error)
            *error = sel.lastError().databaseText();
        return false;
    }
    struct Fix {
        QString site;
        qint64 from = 0;
        qint64 to = 0;
    };
    QVector<Fix> fixes;
    while (sel.next()) {
        const QString site = sel.value(0).toString();
        const qint64 pid = sel.value(1).toLongLong();
        const QString fileUrl = sel.value(2).toString();
        const QString postUrl = sel.value(3).toString();
        const bool isRemote = site != QStringLiteral("local");
        // Local rows key by backing path (post_url column); remote rows
        // by API id, id-less strays by URL hash. Mirrors the pidFor/siteFor
        // split in FavDb.
        const qint64 want =
            wantedPid(isRemote, isRemote ? site : QString(), pid, fileUrl,
                      isRemote ? QString() : postUrl);
        if (want != pid)
            fixes.append({site, pid, want});
    }
    if (fixes.isEmpty())
        return true;
    if (!db.transaction()) {
        if (error)
            *error = db.lastError().databaseText();
        return false;
    }
    QSqlQuery upd(db);
    upd.prepare(QStringLiteral(
        "UPDATE favourites SET post_id=? WHERE site=? AND post_id=?"));
    QSqlQuery del(db);
    del.prepare(QStringLiteral(
        "DELETE FROM favourites WHERE site=? AND post_id=?"));
    for (const Fix &f : fixes) {
        upd.bindValue(0, f.to);
        upd.bindValue(1, f.site);
        upd.bindValue(2, f.from);
        if (upd.exec())
            continue;
        // Target id already taken (same file favourited twice under two
        // identity schemes): the twin survives, this redundant row goes.
        // Anything else (locked DB, I/O) leaves the row for next launch.
        const QString err = upd.lastError().databaseText();
        if (!err.contains(QStringLiteral("UNIQUE"), Qt::CaseInsensitive) &&
            !err.contains(QStringLiteral("PRIMARY"), Qt::CaseInsensitive))
            continue;
        QSqlQuery twin(db);
        twin.prepare(QStringLiteral(
            "SELECT 1 FROM favourites WHERE site=? AND post_id=?"));
        twin.bindValue(0, f.site);
        twin.bindValue(1, f.to);
        if (twin.exec() && twin.next()) {
            del.bindValue(0, f.site);
            del.bindValue(1, f.from);
            del.exec();
        }
    }
    if (!db.commit())
        db.rollback();
    return true;
}

namespace {
bool tableExists(QSqlDatabase &db) {
    QSqlQuery q(db);
    return q.exec(QStringLiteral(
               "SELECT 1 FROM sqlite_master WHERE type='table' AND "
               "name='favourites'")) &&
           q.next();
}

// Temp connection, fully released before returning (removeDatabase with
// a live handle warns and misbehaves - keep the handle block-scoped).
bool withTempDb(const QString &dbPath, const QString &tag,
                const std::function<bool(QSqlDatabase &)> &fn,
                QString *error) {
    const QString conn = QStringLiteral("mig_hash_%1_%2").arg(
        tag, QUuid::createUuid().toString(QUuid::WithoutBraces));
    bool ok = false;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(
            QStringLiteral("QSQLITE"), conn);
        db.setDatabaseName(dbPath);
        if (!db.open()) {
            if (error)
                *error = db.lastError().databaseText();
        } else {
            ok = fn(db);
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(conn);
    return ok;
}
} // namespace

bool needsFavDbHashIds(const QString &dbPath) {
    if (!QFileInfo::exists(dbPath))
        return false;
    bool need = false;
    withTempDb(dbPath, QStringLiteral("probe"),
               [&](QSqlDatabase &db) {
                   // Table presence is the probe: the scan itself is
                   // idempotent, and a present-but-unhealed table is the only
                   // shape that can still hold pre-stable-scheme ids (qHash
                   // rows have no cheap marker - post_id=0 only marks the
                   // older URL-hash era). Dir-scoped done-keys make this a
                   // once-per-data-dir full pass; FavDb::open() keeps the
                   // cheap post_id=0 probe as a mid-session safety net.
                   need = tableExists(db);
                   return true;
               },
               nullptr);
    return need;
}

bool applyFavDbHashIds(const QString &dbPath, QString *error) {
    return withTempDb(
        dbPath, QStringLiteral("apply"),
        [&](QSqlDatabase &db) {
            return !tableExists(db) || healFavDbHashIds(db, error);
        },
        error);
}

} // namespace Migrations
