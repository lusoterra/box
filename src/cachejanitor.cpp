#include "cachejanitor.h"
#include "settings.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QMutexLocker>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QThread>
#include <QUuid>

QRecursiveMutex &CacheJanitor::mutex() {
    static QRecursiveMutex m;
    return m;
}

QString &CacheJanitor::baseDir() {
    static QString d;
    return d;
}

QString &CacheJanitor::trackingDir() {
    static QString d;
    return d;
}

qint64 &CacheJanitor::maxBytes() {
    static qint64 v = qint64(10) * 1024 * 1024 * 1024;
    return v;
}

qint64 &CacheJanitor::lastCheckMs() {
    static qint64 v = 0;
    return v;
}

QString CacheJanitor::dbPath() {
    QMutexLocker lock(&mutex());
    if (!trackingDir().isEmpty())
        return trackingDir() + QStringLiteral("/cache.db");
    // Cache bookkeeping belongs with the cache itself (platform cache
    // dir), not with the data dir - it rebuilds from disk when lost.
    return appCacheDir() + QStringLiteral("/cache.db");
}

namespace {
// One persistent connection per thread. Opened once, never removed:
// removeDatabase() with live handles/queries segfaults Qt's SQLite driver.
thread_local QSqlDatabase t_db;

QSqlDatabase janitorDb() {
    if (t_db.isValid() && t_db.isOpen())
        return t_db;
    QDir().mkpath(QFileInfo(CacheJanitor::dbPath()).absolutePath());
    const QString name = QStringLiteral("janitor_%1").arg(
        QUuid::createUuid().toString(QUuid::WithoutBraces));
    t_db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
    t_db.setDatabaseName(CacheJanitor::dbPath());
    if (!t_db.open())
        return QSqlDatabase();
    QSqlQuery q(t_db);
    if (!q.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS files ("
                               "path TEXT PRIMARY KEY, size INTEGER, seen INTEGER)")))
        return QSqlDatabase();
    return t_db;
}

qint64 nowMs() {
    return QDateTime::currentMSecsSinceEpoch();
}

void putRow(QSqlDatabase &db, const QString &path, qint64 size, qint64 seen) {
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO files (path, size, seen) VALUES (?, ?, ?)"));
    q.addBindValue(path);
    q.addBindValue(size);
    q.addBindValue(seen);
    q.exec();
}
} // namespace

void CacheJanitor::setTrackingDir(const QString &dir) {
    QMutexLocker lock(&mutex());
    if (trackingDir() == dir)
        return;
    trackingDir() = dir;
    // The thread-local handle points at the old database: close it (never
    // removeDatabase - Qt forbids that with live handles) so the next
    // janitorDb() reopens at the new path. Without this, data-dir moves
    // and data-dir moves silently keep accounting in the old cache.db.
    t_db.close();
}

void CacheJanitor::configure(const QString &cacheBaseDir, qint64 capBytes) {
    QMutexLocker lock(&mutex());
    baseDir() = cacheBaseDir;
    maxBytes() = capBytes;
}

void CacheJanitor::record(const QString &path) {
    const QFileInfo fi(path);
    if (!fi.exists())
        return;
    QMutexLocker lock(&mutex());
    QSqlDatabase db = janitorDb();
    if (!db.isOpen())
        return;
    putRow(db, path, fi.size(), nowMs());
}

void CacheJanitor::hit(const QString &path) {
    QMutexLocker lock(&mutex());
    QSqlDatabase db = janitorDb();
    if (!db.isOpen())
        return;
    if (!QFileInfo::exists(path)) {
        // Gone from disk: drop the row so it stops counting.
        QSqlQuery q(db);
        q.prepare(QStringLiteral("DELETE FROM files WHERE path=?"));
        q.addBindValue(path);
        q.exec();
        return;
    }
    QSqlQuery q(db);
    q.prepare(QStringLiteral("UPDATE files SET seen=? WHERE path=?"));
    q.addBindValue(nowMs());
    q.addBindValue(path);
    q.exec();
    if (q.numRowsAffected() <= 0) {
        // Untracked file (pre-DB cache, or DB wiped): adopt it.
        const QFileInfo fi(path);
        putRow(db, path, fi.size(), nowMs());
    }
}

void CacheJanitor::enforce(bool force) {
    QMutexLocker lock(&mutex());
    const qint64 now = nowMs();
    if (!force && now - lastCheckMs() < 60000)
        return;
    lastCheckMs() = now;
    const QString base = baseDir();
    const qint64 cap = maxBytes();
    if (base.isEmpty())
        return;
    QSqlDatabase db = janitorDb();
    if (!db.isOpen())
        return;

    // Adopt: every file on disk gets a row (seen = mtime for newcomers).
    QSet<QString> onDisk;
    for (const QString &sub : {QStringLiteral("thumbs"), QStringLiteral("full")}) {
        QDirIterator it(base + u'/' + sub, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            it.next();
            onDisk.insert(it.fileInfo().absoluteFilePath());
        }
    }
    QSet<QString> tracked;
    {
        QSqlQuery q(db);
        if (q.exec(QStringLiteral("SELECT path FROM files"))) {
            while (q.next())
                tracked.insert(q.value(0).toString());
        }
    }
    for (const QString &p : onDisk) {
        if (tracked.contains(p))
            continue;
        const QFileInfo fi(p);
        putRow(db, p, fi.size(), fi.lastModified().toMSecsSinceEpoch());
    }
    // Drop rows for files that no longer exist.
    {
        QSqlQuery del(db);
        del.prepare(QStringLiteral("DELETE FROM files WHERE path=?"));
        for (const QString &p : tracked) {
            if (!onDisk.contains(p)) {
                del.addBindValue(p);
                del.exec();
            }
        }
    }

    if (cap > 0) {
        // Rotate: oldest-seen first, stop back under 90% (no one-by-one churn).
        qint64 total = 0;
        QList<QPair<QString, qint64>> rows; // path, size (ordered by seen)
        {
            QSqlQuery q(db);
            if (q.exec(QStringLiteral("SELECT path, size FROM files ORDER BY seen ASC"))) {
                while (q.next()) {
                    rows.append({q.value(0).toString(), q.value(1).toLongLong()});
                    total += q.value(1).toLongLong();
                }
            }
        }
        const qint64 target = static_cast<qint64>(cap * 0.9);
        QSqlQuery del(db);
        del.prepare(QStringLiteral("DELETE FROM files WHERE path=?"));
        for (const auto &[path, recorded] : rows) {
            Q_UNUSED(recorded);
            if (total <= target)
                break;
            const QFileInfo fi(path);
            const qint64 actual = fi.exists() ? fi.size() : 0;
            if (QFile::remove(path) || !fi.exists())
                total -= actual;
            del.addBindValue(path);
            del.exec();
        }
    }
}
