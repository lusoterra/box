#include "dedupdb.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStringList>
#include <QUuid>

namespace {
constexpr int kSchemaVersion = 1;
constexpr const char *kConnectionPrefix = "dedupdb_";
} // namespace

DedupDb::~DedupDb() {
    close();
}

bool DedupDb::exec(const QString &sql) const {
    QSqlQuery q(QSqlDatabase::database(m_conn));
    if (q.exec(sql))
        return true;
    m_error = q.lastError().text();
    return false;
}

bool DedupDb::open(const QString &dbPath) {
    close();
    if (dbPath.trimmed().isEmpty()) {
        m_error = QStringLiteral("empty database path");
        return false;
    }
    m_path = QFileInfo(dbPath).absoluteFilePath();
    if (!QDir().mkpath(QFileInfo(m_path).absolutePath())) {
        m_error = QStringLiteral("cannot create %1").arg(
            QFileInfo(m_path).absolutePath());
        return false;
    }
    m_conn = QString::fromLatin1(kConnectionPrefix) +
             QUuid::createUuid().toString(QUuid::WithoutBraces);
    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                m_conn);
    db.setDatabaseName(m_path);
    if (!db.open()) {
        m_error = db.lastError().text();
        QSqlDatabase::removeDatabase(m_conn);
        m_conn.clear();
        return false;
    }
    exec(QStringLiteral("PRAGMA synchronous=NORMAL"));
    exec(QStringLiteral("PRAGMA temp_store=MEMORY"));

    if (!exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS meta (k TEXT PRIMARY KEY, v TEXT)")) ||
        !exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS files ("
            "id INTEGER PRIMARY KEY,"
            "scope TEXT NOT NULL,"
            "root TEXT NOT NULL DEFAULT '',"
            "path TEXT NOT NULL,"
            "size INTEGER NOT NULL DEFAULT 0,"
            "mtime INTEGER NOT NULL DEFAULT 0,"
            "kind TEXT NOT NULL DEFAULT 'image',"
            "sha256 TEXT NOT NULL DEFAULT '',"
            "phash INTEGER NOT NULL DEFAULT 0,"
            "has_phash INTEGER NOT NULL DEFAULT 0,"
            "width INTEGER NOT NULL DEFAULT 0,"
            "height INTEGER NOT NULL DEFAULT 0,"
            "hashed_ms INTEGER NOT NULL DEFAULT 0,"
            "UNIQUE(scope, path))"))) {
        close();
        return false;
    }

    // A schema change invalidates the cache rather than migrating it:
    // the rows are cheap to regenerate and never authoritative.
    int storedSchema = -1;
    {
        QSqlQuery v(QSqlDatabase::database(m_conn));
        if (v.exec(QStringLiteral("SELECT v FROM meta WHERE k='schema'")) &&
            v.next())
            storedSchema = v.value(0).toInt();
    }
    if (storedSchema >= 0 && storedSchema != kSchemaVersion) {
        if (!exec(QStringLiteral("DROP TABLE IF EXISTS files")) ||
            !exec(QStringLiteral(
                "CREATE TABLE files ("
                "id INTEGER PRIMARY KEY,"
                "scope TEXT NOT NULL,"
                "root TEXT NOT NULL DEFAULT '',"
                "path TEXT NOT NULL,"
                "size INTEGER NOT NULL DEFAULT 0,"
                "mtime INTEGER NOT NULL DEFAULT 0,"
                "kind TEXT NOT NULL DEFAULT 'image',"
                "sha256 TEXT NOT NULL DEFAULT '',"
                "phash INTEGER NOT NULL DEFAULT 0,"
                "has_phash INTEGER NOT NULL DEFAULT 0,"
                "width INTEGER NOT NULL DEFAULT 0,"
                "height INTEGER NOT NULL DEFAULT 0,"
                "hashed_ms INTEGER NOT NULL DEFAULT 0,"
                "UNIQUE(scope, path))"))) {
            close();
            return false;
        }
    }
    if (!exec(QStringLiteral(
            "CREATE INDEX IF NOT EXISTS idx_dedup_sha "
            "ON files(scope, kind, sha256)")) ||
        !exec(QStringLiteral(
            "CREATE INDEX IF NOT EXISTS idx_dedup_phash "
            "ON files(scope, kind, phash)")) ||
        !exec(QStringLiteral(
            "INSERT OR REPLACE INTO meta (k, v) VALUES ('schema', '%1')")
                  .arg(kSchemaVersion))) {
        close();
        return false;
    }
    m_open = true;
    return true;
}

void DedupDb::close() {
    if (!m_conn.isEmpty()) {
        {
            QSqlDatabase db = QSqlDatabase::database(m_conn, false);
            if (db.isOpen())
                db.close();
        }
        QSqlDatabase::removeDatabase(m_conn);
        m_conn.clear();
    }
    m_open = false;
    m_transaction = false;
}

QVector<DedupFile> DedupDb::files(const QString &scope) const {
    QVector<DedupFile> out;
    if (!m_open)
        return out;
    QSqlQuery q(QSqlDatabase::database(m_conn));
    if (scope.isEmpty()) {
        if (!q.exec(QStringLiteral(
                "SELECT id, scope, root, path, size, mtime, kind, sha256, "
                "phash, has_phash, width, height, hashed_ms "
                "FROM files ORDER BY id"))) {
            m_error = q.lastError().text();
            return out;
        }
    } else {
        q.prepare(QStringLiteral(
            "SELECT id, scope, root, path, size, mtime, kind, sha256, "
            "phash, has_phash, width, height, hashed_ms "
            "FROM files WHERE scope=? ORDER BY id"));
        q.addBindValue(scope);
        if (!q.exec()) {
            m_error = q.lastError().text();
            return out;
        }
    }
    while (q.next()) {
        DedupFile f;
        f.id = q.value(0).toLongLong();
        f.scope = q.value(1).toString();
        f.root = q.value(2).toString();
        f.path = q.value(3).toString();
        f.size = q.value(4).toLongLong();
        f.mtime = q.value(5).toLongLong();
        f.kind = q.value(6).toString();
        f.sha256 = q.value(7).toString();
        f.phash = static_cast<quint64>(q.value(8).toLongLong());
        f.hasPhash = q.value(9).toInt() != 0;
        f.width = q.value(10).toInt();
        f.height = q.value(11).toInt();
        f.hashedMs = q.value(12).toLongLong();
        out.append(f);
    }
    return out;
}

bool DedupDb::begin() {
    if (!m_open || m_transaction)
        return false;
    if (!QSqlDatabase::database(m_conn).transaction()) {
        m_error = QStringLiteral("cannot start transaction");
        return false;
    }
    m_transaction = true;
    return true;
}

bool DedupDb::commit() {
    if (!m_open || !m_transaction)
        return false;
    m_transaction = false;
    if (!QSqlDatabase::database(m_conn).commit()) {
        m_error = QStringLiteral("commit failed");
        return false;
    }
    return true;
}

bool DedupDb::rollback() {
    if (!m_open || !m_transaction)
        return false;
    m_transaction = false;
    return QSqlDatabase::database(m_conn).rollback();
}

bool DedupDb::upsert(const DedupFile &file) {
    if (!m_open || file.scope.isEmpty() || file.path.isEmpty())
        return false;
    QSqlQuery q(QSqlDatabase::database(m_conn));
    q.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO files "
        "(scope, root, path, size, mtime, kind, sha256, phash, has_phash, "
        "width, height, hashed_ms) VALUES (?,?,?,?,?,?,?,?,?,?,?,?)"));
    q.addBindValue(file.scope);
    q.addBindValue(file.root);
    q.addBindValue(file.path);
    q.addBindValue(file.size);
    q.addBindValue(file.mtime);
    q.addBindValue(file.kind);
    q.addBindValue(file.sha256);
    q.addBindValue(static_cast<qint64>(file.phash));
    q.addBindValue(file.hasPhash ? 1 : 0);
    q.addBindValue(file.width);
    q.addBindValue(file.height);
    q.addBindValue(file.hashedMs);
    if (q.exec())
        return true;
    m_error = q.lastError().text();
    return false;
}

bool DedupDb::removePath(const QString &scope, const QString &path) {
    if (!m_open)
        return false;
    QSqlQuery q(QSqlDatabase::database(m_conn));
    q.prepare(QStringLiteral("DELETE FROM files WHERE scope=? AND path=?"));
    q.addBindValue(scope);
    q.addBindValue(path);
    if (q.exec())
        return true;
    m_error = q.lastError().text();
    return false;
}

bool DedupDb::removeId(qint64 id) {
    if (!m_open)
        return false;
    QSqlQuery q(QSqlDatabase::database(m_conn));
    q.prepare(QStringLiteral("DELETE FROM files WHERE id=?"));
    q.addBindValue(id);
    if (q.exec())
        return true;
    m_error = q.lastError().text();
    return false;
}

void DedupDb::prune(const QString &scope, const QString &root,
                    const QSet<QString> &seen) {
    if (!m_open)
        return;
    if (seen.isEmpty()) {
        QSqlQuery q(QSqlDatabase::database(m_conn));
        q.prepare(QStringLiteral(
            "DELETE FROM files WHERE scope=? AND root=?"));
        q.addBindValue(scope);
        q.addBindValue(root);
        if (!q.exec())
            m_error = q.lastError().text();
        return;
    }
    QStringList stale;
    {
        QSqlQuery q(QSqlDatabase::database(m_conn));
        q.prepare(QStringLiteral(
            "SELECT path FROM files WHERE scope=? AND root=?"));
        q.addBindValue(scope);
        q.addBindValue(root);
        if (!q.exec()) {
            m_error = q.lastError().text();
            return;
        }
        while (q.next()) {
            const QString path = q.value(0).toString();
            if (!seen.contains(path))
                stale.append(path);
        }
    }
    if (stale.isEmpty())
        return;
    if (!begin())
        return;
    QSqlQuery del(QSqlDatabase::database(m_conn));
    del.prepare(QStringLiteral(
        "DELETE FROM files WHERE scope=? AND path=?"));
    for (const QString &path : stale) {
        del.bindValue(0, scope);
        del.bindValue(1, path);
        if (!del.exec())
            m_error = del.lastError().text();
    }
    commit();
}

bool DedupDb::clear() {
    if (!m_open)
        return false;
    if (!exec(QStringLiteral("DELETE FROM files")))
        return false;
    return exec(QStringLiteral("VACUUM"));
}

int DedupDb::count() const {
    if (!m_open)
        return 0;
    QSqlQuery q(QSqlDatabase::database(m_conn));
    if (q.exec(QStringLiteral("SELECT COUNT(*) FROM files")) && q.next())
        return q.value(0).toInt();
    return 0;
}
