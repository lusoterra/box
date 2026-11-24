#include "tagcountcache.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QUuid>
#include <QVariant>

#include <algorithm>

namespace {
QString cacheConnName(const QString &dbPath) {
    return QStringLiteral("tagcount_%1_%2").arg(
        QString::fromLatin1(QCryptographicHash::hash(dbPath.toUtf8(),
                                                     QCryptographicHash::Sha1)
                                .toHex()
                                .left(8)),
        QUuid::createUuid().toString(QUuid::WithoutBraces));
}
} // namespace

QString TagCountCache::dbPath(const QString &dataDir) {
    return dataDir + QStringLiteral("/tags.db");
}

bool TagCountCache::open(const QString &dbPath) {
    close();
    QDir().mkpath(QFileInfo(dbPath).absolutePath());
    m_conn = cacheConnName(dbPath);
    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_conn);
    db.setDatabaseName(dbPath);
    if (!db.open()) {
        QSqlDatabase::removeDatabase(m_conn);
        m_conn.clear();
        return false;
    }
    // Pragmas: suggestions are read-mostly, one writer at a time.
    {
        QSqlQuery q(db);
        q.exec(QStringLiteral("PRAGMA journal_mode=WAL"));
        q.exec(QStringLiteral("PRAGMA synchronous=NORMAL"));
    }
    QSqlQuery q(db);
    // Migration: pre-existing files (none expected, but a future schema
    // change lands here) are upgraded in place by IF NOT EXISTS.
    if (!q.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS tags ("
            "name TEXT PRIMARY KEY COLLATE NOCASE,"
            "tag TEXT NOT NULL,"
            "bucket TEXT NOT NULL DEFAULT 'general',"
            "count INTEGER NOT NULL DEFAULT 0,"
            "updated INTEGER NOT NULL DEFAULT 0)"))) {
        close();
        return false;
    }
    if (!q.exec(QStringLiteral(
            "CREATE INDEX IF NOT EXISTS tags_count ON tags(count DESC)"))) {
        close();
        return false;
    }
    m_open = true;
    // Cap the vocabulary: drop the least popular rows when the table has
    // grown past the limit (keeps prefix queries small and fast).
    const int rows = size();
    if (rows > kMaxRows) {
        QSqlQuery trim(db);
        trim.prepare(QStringLiteral(
            "DELETE FROM tags WHERE name IN ("
            "SELECT name FROM tags ORDER BY count ASC, updated ASC LIMIT ?)"));
        trim.addBindValue(rows - kMaxRows);
        trim.exec();
    }
    // Drop zero-post rows: aliases/empties other engines reported with
    // count 0 (and old unknown-count rows). They never complete a search;
    // anything real is re-reported with its count on next use, and the
    // newest spelling/bucket already won via upsert's MAX().
    {
        QSqlQuery q(db);
        q.exec(QStringLiteral("DELETE FROM tags WHERE count <= 0"));
    }
    return true;
}

void TagCountCache::close() {
    if (m_conn.isEmpty())
        return;
    {
        QSqlDatabase db = QSqlDatabase::database(m_conn, false);
        if (db.isOpen())
            db.close();
    }
    QSqlDatabase::removeDatabase(m_conn);
    m_conn.clear();
    m_open = false;
}

void TagCountCache::upsert(const QVector<CachedTag> &rows) {
    if (!m_open || rows.isEmpty())
        return;
    QSqlDatabase db = QSqlDatabase::database(m_conn, false);
    if (!db.isOpen())
        return;
    const bool ownTxn = db.transaction();
    QSqlQuery q(db);
    // MAX() keeps the popular value: a fresh answer that skips the count
    // (0) or reports fewer posts never demotes a tag.
    q.prepare(QStringLiteral(
        "INSERT INTO tags (name, tag, bucket, count, updated) "
        "VALUES (?, ?, ?, ?, ?) "
        "ON CONFLICT(name) DO UPDATE SET "
        "tag = excluded.tag, bucket = excluded.bucket, "
        "count = MAX(tags.count, excluded.count), "
        "updated = excluded.updated"));
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    for (const CachedTag &r : rows) {
        const QString key = r.tag.trimmed().toLower();
        if (key.isEmpty())
            continue;
        q.addBindValue(key);
        q.addBindValue(r.tag.trimmed());
        q.addBindValue(r.bucket.isEmpty() ? QStringLiteral("general")
                                          : r.bucket);
        q.addBindValue(qMax(0, r.count));
        q.addBindValue(now);
        q.exec();
        q.finish();
    }
    if (ownTxn)
        db.commit();
}

QVector<CachedTag> TagCountCache::suggest(const QString &prefix, int limit,
                                          const QSet<QString> &favTags) const {
    QVector<CachedTag> out;
    if (!m_open || limit <= 0)
        return out;
    const QString q = prefix.trimmed().toLower();
    if (q.isEmpty())
        return out;
    QSqlDatabase db = QSqlDatabase::database(m_conn, false);
    if (!db.isOpen())
        return out;
    // Dense pool: favourites are lifted above counts below, so ask for a
    // few extra rows instead of only the top `limit`.
    QSqlQuery query(db);
    query.prepare(QStringLiteral(
        "SELECT tag, bucket, count, updated FROM tags WHERE name LIKE ? "
        "ORDER BY count DESC, length(tag) ASC, tag ASC LIMIT ?"));
    query.addBindValue(q + u'%');
    query.addBindValue(qMax(limit, qMin(64, limit * 3)));
    if (!query.exec())
        return out;
    while (query.next()) {
        CachedTag t;
        t.tag = query.value(0).toString();
        t.bucket = query.value(1).toString();
        t.count = query.value(2).toInt();
        t.updated = query.value(3).toLongLong();
        out.append(t);
    }
    // Same relevance contract as the rest of the suggestion stack: exact
    // tag first, then favourite prefix matches, then plain prefix matches
    // by post count.
    std::stable_sort(out.begin(), out.end(),
                     [&q, &favTags](const CachedTag &a, const CachedTag &b) {
                         const auto tierOf = [&q, &favTags](const QString &t) {
                             const QString l = t.toLower();
                             if (l == q)
                                 return 0;
                             if (favTags.contains(l))
                                 return 1;
                             return 2;
                         };
                         const int at = tierOf(a.tag), bt = tierOf(b.tag);
                         if (at != bt)
                             return at < bt;
                         if (a.count != b.count)
                             return a.count > b.count;
                         if (a.tag.size() != b.tag.size())
                             return a.tag.size() < b.tag.size();
                         return a.tag < b.tag;
                     });
    if (out.size() > limit)
        out.resize(limit);
    return out;
}

int TagCountCache::size() const {
    if (!m_open)
        return 0;
    QSqlQuery q(QSqlDatabase::database(m_conn, false));
    if (!q.exec(QStringLiteral("SELECT COUNT(*) FROM tags")) || !q.next())
        return 0;
    return q.value(0).toInt();
}
