#include "hydrusstore.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QUrl>
#include <QUuid>

#include <algorithm>

namespace HydrusStore {

SplitTag splitTag(const QString &tag) {
    SplitTag out;
    const QString t = tag.trimmed();
    const qsizetype colon = t.indexOf(u':');
    if (colon > 0) {
        out.ns = t.left(colon).trimmed().toLower();
        out.tag = t.mid(colon + 1).trimmed();
        if (out.ns.isEmpty() || out.tag.isEmpty()) {
            out.ns.clear();
            out.tag = t;
        }
    } else {
        out.tag = t;
    }
    return out;
}

QString normalizeTag(const QString &raw) {
    QString t = raw.trimmed().toLower();
    if (t.isEmpty())
        return {};
    // Hydrus allows spaces in tag names; the app's search tokens are
    // space-separated, so fold every run to '_' (Hydrus's own PTR style).
    QString folded;
    folded.reserve(t.size());
    bool inSpace = false;
    for (const QChar c : t) {
        if (c.isSpace()) {
            inSpace = true;
            continue;
        }
        if (inSpace && !folded.isEmpty())
            folded += u'_';
        inSpace = false;
        folded += c;
    }
    while (folded.endsWith(u'_'))
        folded.chop(1);
    if (folded.startsWith(u':'))
        folded = folded.mid(1);
    if (folded.isEmpty())
        return {};
    const qsizetype colon = folded.indexOf(u':');
    if (colon > 0 && colon + 1 >= folded.size())
        return {}; // "creator:" is not a tag
    return folded;
}

QStringList normalizeTags(const QStringList &raw) {
    QStringList out;
    QSet<QString> seen;
    for (const QString &r : raw) {
        const QString t = normalizeTag(r);
        if (t.isEmpty() || seen.contains(t))
            continue;
        seen.insert(t);
        out.append(t);
    }
    std::sort(out.begin(), out.end(), [](const QString &a, const QString &b) {
        return a.compare(b, Qt::CaseInsensitive) < 0;
    });
    return out;
}

QMap<QString, QStringList> groupByNamespace(const QStringList &tags) {
    QMap<QString, QStringList> out;
    for (const QString &t : tags) {
        const SplitTag s = splitTag(t);
        QStringList &list = out[s.ns];
        if (!list.contains(s.tag))
            list.append(s.tag);
    }
    return out;
}

QString fileNameForHash(const QString &sha256, const QString &ext) {
    QString e = ext.trimmed().toLower();
    if (e.startsWith(u'.'))
        e = e.mid(1);
    const QString h = sha256.trimmed().toLower();
    return e.isEmpty() ? h : h + u'.' + e;
}

bool isSha256Hex(const QString &s) {
    const QString t = s.trimmed();
    if (t.size() != 64)
        return false;
    for (const QChar c : t) {
        const ushort u = c.unicode();
        if (!((u >= u'0' && u <= u'9') || (u >= u'a' && u <= u'f') ||
              (u >= u'A' && u <= u'F')))
            return false;
    }
    return true;
}

QStringList applySpamFilter(const QStringList &tags,
                            const QHash<QString, int> &cachedCounts,
                            const SpamFilter &filter) {
    const QStringList normalized = normalizeTags(tags);
    if (filter.minPosts <= 0)
        return normalized;
    QStringList kept;
    for (const QString &tag : normalized) {
        const SplitTag s = splitTag(tag);
        int best = -1;
        const auto check = [&](const QString &key) {
            const auto it = cachedCounts.constFind(key.trimmed().toLower());
            if (it == cachedCounts.constEnd())
                return;
            best = qMax(best, it.value());
        };
        check(tag);
        if (s.namespaced())
            check(s.tag);
        if (best >= filter.minPosts)
            kept.append(tag);
        else if (best < 0 && !filter.dropUnknown)
            kept.append(tag);
        // Known but under the threshold: spam, drop.
    }
    return kept;
}

Post Record::toPost(const QString &mediaDir) const {
    Post p;
    p.file = fileName;
    p.absFile = QDir(mediaDir).filePath(fileName);
    p.ext = ext;
    if (p.ext.isEmpty())
        p.ext = QFileInfo(fileName).suffix().toLower();
    p.width = width;
    p.height = height;
    p.id = 0;
    p.sha256 = sha256;
    p.siteKey = QStringLiteral("hydrus");
    p.siteName = QStringLiteral("Hydrus");
    p.siteShort = QStringLiteral("HYD");
    p.source = source;
    if (source.startsWith(QStringLiteral("http://"), Qt::CaseInsensitive) ||
        source.startsWith(QStringLiteral("https://"), Qt::CaseInsensitive)) {
        p.postUrl = source;
    }
    if (addedMs > 0)
        p.date = QDateTime::fromMSecsSinceEpoch(addedMs).toString(Qt::ISODate);

    QStringList search;
    QSet<QString> seen;
    QStringList general;
    QMap<QString, QStringList> groups;
    QString rating;
    QString title;
    for (const QString &tag : normalizeTags(tags)) {
        const SplitTag s = splitTag(tag);
        const auto add = [&seen, &search](const QString &v) {
            if (!v.isEmpty() && !seen.contains(v)) {
                seen.insert(v);
                search.append(v);
            }
        };
        add(tag);
        if (s.namespaced()) {
            add(s.tag);
            groups[s.ns].append(s.tag);
            if (s.ns == QStringLiteral("rating") && rating.isEmpty())
                rating = s.tag;
            if (s.ns == QStringLiteral("title") && title.isEmpty())
                title = s.tag;
        } else {
            general.append(s.tag);
        }
    }
    p.tags = search.join(u' ');
    p.tagsGeneral = general.join(u' ');
    for (auto it = groups.begin(); it != groups.end(); ++it) {
        if (it.key().isEmpty() || it.value().isEmpty())
            continue;
        p.customTags.insert(it.key(), it.value());
    }
    if (!rating.isEmpty()) {
        // Hydrus PTR rating tags use safe/questionable/explicit; the app's
        // rating model calls "safe" general.
        p.rating = rating == QStringLiteral("safe")
                       ? QStringLiteral("general")
                       : rating;
    }
    p.title = title;
    p.hasMeta = !p.tags.isEmpty();
    return p;
}

namespace {
QSqlDatabase dbFor(const QString &conn) {
    return QSqlDatabase::database(conn, false);
}

Record recordFromQuery(const QSqlQuery &q) {
    Record r;
    r.sha256 = q.value(0).toString();
    r.fileName = q.value(1).toString();
    r.ext = q.value(2).toString();
    r.bytes = q.value(3).toLongLong();
    r.width = q.value(4).toInt();
    r.height = q.value(5).toInt();
    r.addedMs = q.value(6).toLongLong();
    r.source = q.value(7).toString();
    r.hydrusFileId = q.value(8).toLongLong();
    r.found = q.value(9).toInt() != 0;
    r.manualOverride = q.value(10).toInt() != 0;
    r.lastLookupMs = q.value(11).toLongLong();
    r.tags = normalizeTags(q.value(12).toString().split(
        QChar::Space, Qt::SkipEmptyParts));
    return r;
}

const char *kSelectColumns =
    "sha256, file_name, ext, bytes, width, height, added_ms, source, "
    "hydrus_file_id, hydrus_found, manual_override, last_lookup_ms, tags";

void bindRecord(QSqlQuery &q, const Record &r) {
    const auto text = [](const QString &s) {
        return s.isNull() ? QStringLiteral("") : s;
    };
    q.addBindValue(r.sha256.trimmed().toLower());
    q.addBindValue(text(r.fileName));
    q.addBindValue(text(r.ext));
    q.addBindValue(r.bytes);
    q.addBindValue(r.width);
    q.addBindValue(r.height);
    q.addBindValue(r.addedMs);
    q.addBindValue(text(r.source));
    q.addBindValue(r.hydrusFileId);
    q.addBindValue(r.found ? 1 : 0);
    q.addBindValue(r.manualOverride ? 1 : 0);
    q.addBindValue(r.lastLookupMs);
    q.addBindValue(text(normalizeTags(r.tags).join(u' ')));
}
} // namespace

bool Db::exec(const QString &sql) const {
    QSqlQuery q(dbFor(m_conn));
    return q.exec(sql);
}

bool Db::open(const QString &dbPath) {
    close();
    if (dbPath.trimmed().isEmpty())
        return false;
    const QFileInfo fi(dbPath);
    if (!QDir().mkpath(fi.absolutePath()))
        return false;
    m_path = dbPath;
    m_conn = QStringLiteral("hydrus_%1").arg(
        QUuid::createUuid().toString(QUuid::WithoutBraces));
    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_conn);
    db.setDatabaseName(dbPath);
    if (!db.open()) {
        QSqlDatabase::removeDatabase(m_conn);
        m_conn.clear();
        m_path.clear();
        return false;
    }
    {
        QSqlQuery q(db);
        q.exec(QStringLiteral("PRAGMA journal_mode=WAL"));
        q.exec(QStringLiteral("PRAGMA synchronous=NORMAL"));
    }
    if (!exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS posts ("
            "sha256 TEXT PRIMARY KEY,"
            "file_name TEXT NOT NULL,"
            "ext TEXT NOT NULL DEFAULT '',"
            "bytes INTEGER NOT NULL DEFAULT 0,"
            "width INTEGER NOT NULL DEFAULT 0,"
            "height INTEGER NOT NULL DEFAULT 0,"
            "added_ms INTEGER NOT NULL DEFAULT 0,"
            "source TEXT NOT NULL DEFAULT '',"
            "hydrus_file_id INTEGER NOT NULL DEFAULT 0,"
            "hydrus_found INTEGER NOT NULL DEFAULT 0,"
            "manual_override INTEGER NOT NULL DEFAULT 0,"
            "last_lookup_ms INTEGER NOT NULL DEFAULT 0,"
            "tags TEXT NOT NULL DEFAULT '')")) ||
        !exec(QStringLiteral(
            "CREATE INDEX IF NOT EXISTS posts_added ON posts(added_ms DESC)"))) {
        close();
        return false;
    }
    m_open = true;
    return true;
}

void Db::close() {
    if (m_conn.isEmpty())
        return;
    {
        QSqlDatabase db = QSqlDatabase::database(m_conn, false);
        if (db.isOpen())
            db.close();
    }
    QSqlDatabase::removeDatabase(m_conn);
    m_conn.clear();
    m_path.clear();
    m_open = false;
}

int Db::count() const {
    if (!m_open)
        return 0;
    QSqlQuery q(dbFor(m_conn));
    if (!q.exec(QStringLiteral("SELECT COUNT(*) FROM posts")) || !q.next())
        return 0;
    return q.value(0).toInt();
}

bool Db::contains(const QString &sha256) const {
    if (!m_open)
        return false;
    QSqlQuery q(dbFor(m_conn));
    q.prepare(QStringLiteral("SELECT 1 FROM posts WHERE sha256 = ?"));
    q.addBindValue(sha256.trimmed().toLower());
    return q.exec() && q.next();
}

Record Db::record(const QString &sha256) const {
    if (!m_open)
        return {};
    QSqlQuery q(dbFor(m_conn));
    q.prepare(QStringLiteral("SELECT ") + QString::fromLatin1(kSelectColumns) +
              QStringLiteral(" FROM posts WHERE sha256 = ?"));
    q.addBindValue(sha256.trimmed().toLower());
    if (!q.exec() || !q.next())
        return {};
    return recordFromQuery(q);
}

QVector<Record> Db::list() const {
    QVector<Record> out;
    if (!m_open)
        return out;
    QSqlQuery q(dbFor(m_conn));
    if (!q.exec(QStringLiteral("SELECT ") +
                QString::fromLatin1(kSelectColumns) +
                QStringLiteral(" FROM posts ORDER BY added_ms DESC, sha256 ASC")))
        return out;
    while (q.next())
        out.append(recordFromQuery(q));
    return out;
}

bool Db::insert(const Record &r) {
    if (!m_open)
        return false;
    const QString sha = r.sha256.trimmed().toLower();
    if (sha.isEmpty() || contains(sha))
        return false;
    QSqlQuery q(dbFor(m_conn));
    q.prepare(QStringLiteral(
        "INSERT INTO posts (sha256, file_name, ext, bytes, width, "
        "height, added_ms, source, hydrus_file_id, hydrus_found, "
        "manual_override, last_lookup_ms, tags) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?)"));
    bindRecord(q, r);
    const bool ok = q.exec();
    return ok && q.numRowsAffected() > 0;
}

bool Db::updateTags(const QString &sha256, const QStringList &tags, bool found,
                    qint64 fileId, qint64 lookupMs, bool clearOverride) {
    if (!m_open)
        return false;
    QSqlQuery q(dbFor(m_conn));
    if (clearOverride) {
        q.prepare(QStringLiteral(
            "UPDATE posts SET tags = ?, hydrus_found = ?, "
            "hydrus_file_id = ?, last_lookup_ms = ?, manual_override = 0 "
            "WHERE sha256 = ?"));
    } else {
        q.prepare(QStringLiteral(
            "UPDATE posts SET tags = ?, hydrus_found = ?, "
            "hydrus_file_id = ?, last_lookup_ms = ? WHERE sha256 = ?"));
    }
    q.addBindValue(normalizeTags(tags).join(u' '));
    q.addBindValue(found ? 1 : 0);
    q.addBindValue(fileId);
    q.addBindValue(lookupMs);
    q.addBindValue(sha256.trimmed().toLower());
    return q.exec() && q.numRowsAffected() > 0;
}

bool Db::updateManualTags(const QString &sha256, const QStringList &tags) {
    if (!m_open)
        return false;
    QSqlQuery q(dbFor(m_conn));
    q.prepare(QStringLiteral(
        "UPDATE posts SET tags = ?, manual_override = 1 WHERE sha256 = ?"));
    q.addBindValue(normalizeTags(tags).join(u' '));
    q.addBindValue(sha256.trimmed().toLower());
    return q.exec() && q.numRowsAffected() > 0;
}

bool Db::markMissed(const QString &sha256) {
    if (!m_open)
        return false;
    QSqlQuery q(dbFor(m_conn));
    q.prepare(QStringLiteral(
        "UPDATE posts SET hydrus_found = 0, last_lookup_ms = ? "
        "WHERE sha256 = ?"));
    q.addBindValue(QDateTime::currentMSecsSinceEpoch());
    q.addBindValue(sha256.trimmed().toLower());
    return q.exec() && q.numRowsAffected() > 0;
}

bool Db::updateFile(const QString &sha256, const QString &fileName,
                    const QString &ext, qint64 bytes) {
    if (!m_open)
        return false;
    QSqlQuery q(dbFor(m_conn));
    q.prepare(QStringLiteral(
        "UPDATE posts SET file_name = ?, ext = ?, bytes = ? WHERE sha256 = ?"));
    q.addBindValue(fileName);
    q.addBindValue(ext);
    q.addBindValue(bytes);
    q.addBindValue(sha256.trimmed().toLower());
    return q.exec() && q.numRowsAffected() > 0;
}

} // namespace HydrusStore
