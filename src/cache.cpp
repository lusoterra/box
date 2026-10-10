#include "cache.h"
#include "settings.h"

#include <QCryptographicHash>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>

namespace {

constexpr int kVersion = 2; // v2: posts carry the "untyped" tag list

QString cacheDirFor(const QString &rootAbs) {
    const QString base = appCacheDir();
    const QString hash =
        QString::fromLatin1(QCryptographicHash::hash(rootAbs.toUtf8(),
                                                     QCryptographicHash::Sha1)
                                 .toHex()
                                 .left(12));
    return base + u'/' + QStringLiteral("roots") + u'/' + hash;
}

QJsonObject postToJson(const Post &p) {
    QJsonObject o;
    o[QStringLiteral("id")] = p.id;
    o[QStringLiteral("score")] = p.score;
    o[QStringLiteral("rating")] = p.rating;
    o[QStringLiteral("ratingInferred")] = p.ratingInferred;
    o[QStringLiteral("tags")] = p.tags;
    o[QStringLiteral("tagsArtist")] = p.tagsArtist;
    o[QStringLiteral("tagsCharacter")] = p.tagsCharacter;
    o[QStringLiteral("tagsCopyright")] = p.tagsCopyright;
    o[QStringLiteral("tagsMeta")] = p.tagsMeta;
    o[QStringLiteral("tagsGeneral")] = p.tagsGeneral;
    o[QStringLiteral("untyped")] = p.untyped;
    o[QStringLiteral("width")] = p.width;
    o[QStringLiteral("height")] = p.height;
    o[QStringLiteral("source")] = p.source;
    o[QStringLiteral("date")] = p.date;
    o[QStringLiteral("title")] = p.title;
    o[QStringLiteral("postUrl")] = p.postUrl;
    o[QStringLiteral("category")] = p.category;
    o[QStringLiteral("hasMeta")] = p.hasMeta;
    return o;
}

void postFromJson(const QJsonObject &o, Post &p) {
    p.id = o.value(QStringLiteral("id")).toVariant().toLongLong();
    p.score = o.value(QStringLiteral("score")).toInt();
    p.rating = o.value(QStringLiteral("rating")).toString(QStringLiteral("general"));
    p.ratingInferred = o.value(QStringLiteral("ratingInferred")).toBool();
    p.tags = o.value(QStringLiteral("tags")).toString();
    p.tagsArtist = o.value(QStringLiteral("tagsArtist")).toString();
    p.tagsCharacter = o.value(QStringLiteral("tagsCharacter")).toString();
    p.tagsCopyright = o.value(QStringLiteral("tagsCopyright")).toString();
    p.tagsMeta = o.value(QStringLiteral("tagsMeta")).toString();
    p.tagsGeneral = o.value(QStringLiteral("tagsGeneral")).toString();
    p.untyped = o.value(QStringLiteral("untyped")).toString();
    p.width = o.value(QStringLiteral("width")).toInt();
    p.height = o.value(QStringLiteral("height")).toInt();
    p.source = o.value(QStringLiteral("source")).toString();
    p.date = o.value(QStringLiteral("date")).toString();
    p.title = o.value(QStringLiteral("title")).toString();
    p.postUrl = o.value(QStringLiteral("postUrl")).toString();
    p.category = o.value(QStringLiteral("category")).toString();
    p.hasMeta = o.value(QStringLiteral("hasMeta")).toBool();
}

} // namespace

QString ScanCache::dbPathFor(const QString &rootAbs) {
    return cacheDirFor(rootAbs) + u'/' + QStringLiteral("index.db");
}

bool ScanCache::open(const QString &rootAbs, const QString &explicitHash) {
    close();
    const QString dir = cacheDirFor(rootAbs);
    if (!QDir().mkpath(dir))
        return false;
    m_conn = QStringLiteral("scan_%1").arg(
        QString::fromLatin1(QCryptographicHash::hash(rootAbs.toUtf8(),
                                                     QCryptographicHash::Sha1)
                                 .toHex()
                                 .left(8)));
    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_conn);
    db.setDatabaseName(dbPathFor(rootAbs));
    if (!db.open()) {
        QSqlDatabase::removeDatabase(m_conn);
        m_conn.clear();
        return false;
    }
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS meta (k TEXT PRIMARY KEY, v TEXT)")) ||
        !q.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS files ("
                               "path TEXT PRIMARY KEY, mtime INTEGER, size INTEGER, "
                               "post TEXT)")) ||
        !q.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS tag_types ("
                               "tag TEXT PRIMARY KEY, type INTEGER)"))) {
        close();
        return false;
    }
    q.prepare(QStringLiteral("SELECT v FROM meta WHERE k='version'"));
    QString ver;
    if (q.exec() && q.next())
        ver = q.value(0).toString();
    QSqlQuery w(db);
    if (ver != QString::number(kVersion)) {
        w.exec(QStringLiteral("DELETE FROM files"));
        w.prepare(QStringLiteral(
            "INSERT OR REPLACE INTO meta (k, v) VALUES ('version', ?)"));
        w.addBindValue(QString::number(kVersion));
        w.exec();
        // remember which root this db belongs to (debug aid)
        w.prepare(QStringLiteral(
            "INSERT OR REPLACE INTO meta (k, v) VALUES ('root', ?)"));
        w.addBindValue(rootAbs);
        w.exec();
    }
    // Explicit-tag set changed -> stored ratings are stale, re-parse all.
    QString oldHash;
    {
        QSqlQuery h(db);
        h.prepare(QStringLiteral("SELECT v FROM meta WHERE k='explicit'"));
        if (h.exec() && h.next())
            oldHash = h.value(0).toString();
    }
    if (oldHash != explicitHash) {
        w.exec(QStringLiteral("DELETE FROM files"));
        w.prepare(QStringLiteral(
            "INSERT OR REPLACE INTO meta (k, v) VALUES ('explicit', ?)"));
        w.addBindValue(explicitHash);
        w.exec();
    }
    m_open = true;
    return true;
}

bool ScanCache::lookup(const QString &relPath, qint64 mtime, qint64 size, Post &post) {
    if (!m_open)
        return false;
    QSqlQuery q(QSqlDatabase::database(m_conn));
    q.prepare(QStringLiteral("SELECT mtime, size, post FROM files WHERE path=?"));
    q.addBindValue(relPath);
    if (!q.exec() || !q.next()) {
        m_misses++;
        return false;
    }
    if (q.value(0).toLongLong() != mtime || q.value(1).toLongLong() != size) {
        m_misses++;
        return false;
    }
    const QJsonDocument doc =
        QJsonDocument::fromJson(q.value(2).toString().toUtf8());
    if (!doc.isObject()) {
        m_misses++;
        return false;
    }
    postFromJson(doc.object(), post);
    m_hits++;
    return true;
}

void ScanCache::store(const QString &relPath, qint64 mtime, qint64 size, const Post &post) {
    if (!m_open)
        return;
    QSqlQuery q(QSqlDatabase::database(m_conn));
    q.prepare(QStringLiteral("INSERT OR REPLACE INTO files (path, mtime, size, post) "
                             "VALUES (?, ?, ?, ?)"));
    q.addBindValue(relPath);
    q.addBindValue(mtime);
    q.addBindValue(size);
    q.addBindValue(QString::fromUtf8(
        QJsonDocument(postToJson(post)).toJson(QJsonDocument::Compact)));
    q.exec();
}

void ScanCache::finish(const QSet<QString> &seen) {
    if (!m_open)
        return;
    QSqlDatabase db = QSqlDatabase::database(m_conn);
    QSet<QString> stale;
    {
        QSqlQuery sel(db);
        if (sel.exec(QStringLiteral("SELECT path FROM files"))) {
            while (sel.next()) {
                const QString p = sel.value(0).toString();
                if (!seen.contains(p))
                    stale.insert(p);
            }
        }
    }
    // Delete stale rows in chunks to stay under SQLite variable limits.
    const QList<QString> list(stale.begin(), stale.end());
    for (int i = 0; i < list.size(); i += 900) {
            const QList<QString> chunk = list.mid(i, 900);
            QString ph;
            for (int j = 0; j < chunk.size(); ++j)
                ph += (j ? QStringLiteral(",?") : QStringLiteral("?"));
            QSqlQuery del(db);
            del.prepare(QStringLiteral("DELETE FROM files WHERE path IN (") + ph +
                        QStringLiteral(")"));
            for (const QString &p : chunk)
                del.addBindValue(p);
            del.exec();
        }
    close();
}

void ScanCache::close() {
    if (m_conn.isEmpty())
        return;
    {
        QSqlDatabase db = QSqlDatabase::database(m_conn, false);
        if (db.isValid() && db.isOpen())
            db.close();
    }
    QSqlDatabase::removeDatabase(m_conn);
    m_conn.clear();
    m_open = false;
}
