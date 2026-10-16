#include "favdb.h"
#include "explicittags.h"
#include "settings.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QCryptographicHash>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>

#include <cstring>

namespace {
// Stable 63-bit id from a string (SHA1 prefix): unlike qHash, identical
// across processes and Qt versions, so hash-based favourite identities
// survive restarts and upgrades.
qint64 stableId(const QString &s) {
    const QByteArray h =
        QCryptographicHash::hash(s.toUtf8(), QCryptographicHash::Sha1);
    qint64 id = 0;
    memcpy(&id, h.constData(), sizeof(id));
    // Positive only: keeps ids clear of any sentinel range.
    return id & 0x7fffffffffffffffLL;
}

// Native custom categories (category -> tags) as a compact JSON object
// string for the favourites custom column. Empty map <-> empty string.
QString encodeCustom(const QMap<QString, QStringList> &custom) {
    if (custom.isEmpty())
        return {};
    QJsonObject o;
    for (auto it = custom.begin(); it != custom.end(); ++it) {
        QJsonArray tags;
        for (const QString &t : it.value())
            tags.append(t);
        o.insert(it.key(), tags);
    }
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

QMap<QString, QStringList> decodeCustom(const QString &s) {
    QMap<QString, QStringList> out;
    if (s.isEmpty())
        return out;
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(s.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
        return out;
    const QJsonObject o = doc.object();
    for (auto it = o.begin(); it != o.end(); ++it) {
        if (!it.value().isArray())
            continue;
        QStringList tags;
        for (const QJsonValue &v : it.value().toArray()) {
            if (v.isString() && !v.toString().isEmpty())
                tags << v.toString();
        }
        if (!tags.isEmpty())
            out.insert(it.key(), tags);
    }
    return out;
}
} // namespace

qint64 FavDb::pidFor(const Post &post) {
    if (post.siteName.isEmpty() && !post.isRemote)
        return stableId(post.absFile);
    if (post.id != 0)
        return post.id;
    return stableId(post.fileUrl.isEmpty() ? post.absFile : post.fileUrl);
}

void FavDb::migrateHashIds() {
    // One-time healing: rows keyed by unstable identities get stable ones.
    // (site,0) rows predate URL-hash identity; qHash-based rows predate
    // the SHA1 scheme. True API ids never change. True duplicates created
    // by the rewrite collapse to one row.
    QSqlDatabase db = QSqlDatabase::database(m_conn);
    QSqlQuery sel(db);
    if (!sel.exec(QStringLiteral(
            "SELECT site, post_id, file_url, post_url FROM favourites")))
        return;
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
        Post probe;
        probe.isRemote = site != QStringLiteral("local");
        if (probe.isRemote)
            probe.siteName = site;
        probe.id = pid;
        probe.fileUrl = fileUrl;
        if (!probe.isRemote)
            probe.absFile = postUrl;
        const qint64 want = pidFor(probe);
        if (want != pid)
            fixes.append({site, pid, want});
    }
    if (fixes.isEmpty())
        return;
    if (!db.transaction())
        return;
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
}

bool FavDb::open(const QString &dirIn) {
    close();
    QString dir = dirIn.isEmpty() ? defaultDataDir() : dirIn;
    if (!QDir().mkpath(dir))
        return false;
    // Instance-unique: two live instances must never share a connection name
    // (Qt removes the old one out from under live handles -> crash).
    m_conn = QStringLiteral("favdb_%1").arg(
        QUuid::createUuid().toString(QUuid::WithoutBraces));
    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_conn);
    db.setDatabaseName(dir + QStringLiteral("/store.db"));
    if (!db.open()) {
        QSqlDatabase::removeDatabase(m_conn);
        m_conn.clear();
        return false;
    }
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS favourites ("
                               "site TEXT, post_id INTEGER, rating TEXT, score INTEGER, "
                               "ext TEXT, tags TEXT, tags_artist TEXT, tags_character TEXT, "
                               "tags_copyright TEXT, tags_general TEXT, tags_meta TEXT, "
                               "file_url TEXT, sample_url TEXT, preview_url TEXT, "
                               "post_url TEXT, width INTEGER, height INTEGER, "
                               "added INTEGER, local_path TEXT, untyped TEXT, "
                               "custom TEXT, PRIMARY KEY (site, post_id))"))) {
        close();
        return false;
    }
    QSqlQuery alter(db);
    alter.exec(QStringLiteral(
        "ALTER TABLE favourites ADD COLUMN local_path TEXT"));
    alter.exec(QStringLiteral(
        "ALTER TABLE favourites ADD COLUMN untyped TEXT"));
    alter.exec(QStringLiteral(
        "ALTER TABLE favourites ADD COLUMN custom TEXT"));
    Q_UNUSED(alter); // fails when the columns already exist: fine
    // Backfill: rows predating the column get their current general tags
    // as untyped. Correct in both cases: never-classified rows keep the
    // whole blob, and classified rows already moved resolved tags out of
    // general, leaving exactly the still-untyped remainder.
    QSqlQuery backfill(db);
    backfill.exec(QStringLiteral(
        "UPDATE favourites SET untyped=tags_general WHERE untyped IS NULL"));
    migrateHashIds();
    m_open = true;
    return true;
}

bool FavDb::setLocalPath(const QString &site, qint64 postId,
                         const QString &path) {
    if (!m_open)
        return false;
    QSqlQuery q(QSqlDatabase::database(m_conn));
    q.prepare(QStringLiteral(
        "UPDATE favourites SET local_path=? WHERE site=? AND post_id=?"));
    q.addBindValue(path);
    q.addBindValue(site);
    q.addBindValue(postId);
    return q.exec();
}

int FavDb::setLocalPaths(const QHash<QPair<QString, qint64>, QString> &paths) {
    if (!m_open || paths.isEmpty())
        return 0;
    QSqlDatabase db = QSqlDatabase::database(m_conn);
    if (!db.transaction())
        return 0;
    int n = 0;
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "UPDATE favourites SET local_path=? WHERE site=? AND post_id=?"));
    for (auto it = paths.begin(); it != paths.end(); ++it) {
        q.bindValue(0, it.value());
        q.bindValue(1, it.key().first);
        q.bindValue(2, it.key().second);
        if (q.exec())
            n += q.numRowsAffected();
    }
    if (!db.commit()) {
        db.rollback();
        return 0;
    }
    return n;
}

int FavDb::setTagSplits(const QHash<QPair<QString, qint64>, TagSplit> &splits) {
    if (!m_open || splits.isEmpty())
        return 0;
    QSqlDatabase db = QSqlDatabase::database(m_conn);
    if (!db.transaction())
        return 0;
    int n = 0;
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "UPDATE favourites SET tags_artist=?, tags_character=?, "
        "tags_copyright=?, tags_general=?, tags_meta=?, untyped=? "
        "WHERE site=? AND post_id=?"));
    for (auto it = splits.begin(); it != splits.end(); ++it) {
        const TagSplit &s = it.value();
        q.bindValue(0, s.artist);
        q.bindValue(1, s.character);
        q.bindValue(2, s.copyright);
        q.bindValue(3, s.general);
        q.bindValue(4, s.meta);
        q.bindValue(5, s.untyped);
        q.bindValue(6, it.key().first);
        q.bindValue(7, it.key().second);
        if (q.exec())
            n += q.numRowsAffected();
    }
    if (!db.commit()) {
        db.rollback();
        return 0;
    }
    return n;
}

QVector<FavDb::Unlinked> FavDb::unlinked() {
    QVector<Unlinked> out;
    if (!m_open)
        return out;
    // Never linked AND previously linked-but-gone (user moved/renamed
    // files outside the app): both are re-resolvable by the md5 glob in
    // relinkFavourites, so both come back for another attempt.
    QSqlQuery q(QSqlDatabase::database(m_conn));
    if (!q.exec(QStringLiteral("SELECT site, post_id, file_url, local_path, "
                               "tags, ext FROM favourites")))
        return out;
    while (q.next()) {
        const QString local = q.value(3).toString();
        if (!local.isEmpty() && QFileInfo::exists(local))
            continue;
        Unlinked u;
        u.site = q.value(0).toString();
        u.id = q.value(1).toLongLong();
        u.fileUrl = q.value(2).toString();
        u.tags = q.value(4).toString();
        u.ext = q.value(5).toString();
        out.append(u);
    }
    return out;
}

void FavDb::close() {
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

bool FavDb::isFavourite(const QString &site, qint64 postId) {
    if (!m_open)
        return false;
    QSqlQuery q(QSqlDatabase::database(m_conn));
    q.prepare(QStringLiteral("SELECT 1 FROM favourites WHERE site=? AND post_id=?"));
    q.addBindValue(site);
    q.addBindValue(postId);
    return q.exec() && q.next();
}

bool FavDb::add(const Post &post) {
    if (!m_open)
        return false;
    const QString site = FavDb::siteFor(post);
    const qint64 pid = FavDb::pidFor(post);
    if (isFavourite(site, pid))
        return false;
    QSqlQuery q(QSqlDatabase::database(m_conn));
    q.prepare(QStringLiteral(
        "INSERT INTO favourites (site, post_id, rating, score, ext, tags, "
        "tags_artist, tags_character, tags_copyright, tags_general, tags_meta, "
        "file_url, sample_url, preview_url, post_url, width, height, added, untyped, custom) "
        "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"));
    q.addBindValue(site);
    q.addBindValue(pid);
    q.addBindValue(post.rating);
    q.addBindValue(post.score);
    q.addBindValue(post.ext);
    q.addBindValue(post.tags);
    q.addBindValue(post.tagsArtist);
    q.addBindValue(post.tagsCharacter);
    q.addBindValue(post.tagsCopyright);
    q.addBindValue(post.tagsGeneral);
    q.addBindValue(post.tagsMeta);
    q.addBindValue(post.fileUrl);
    q.addBindValue(post.sampleUrl);
    q.addBindValue(post.previewUrl);
    // Local posts are keyed by file: always store the file path, never a
    // source URL from the metadata (rowToPost serves it as absFile).
    q.addBindValue(post.isRemote
                       ? (post.postUrl.isEmpty() ? post.absFile : post.postUrl)
                       : post.absFile);
    q.addBindValue(post.width);
    q.addBindValue(post.height);
    q.addBindValue(QDateTime::currentMSecsSinceEpoch());
    q.addBindValue(post.untyped);
    q.addBindValue(encodeCustom(post.customTags));
    return q.exec();
}

int FavDb::addMany(const QVector<Post> &posts) {
    if (!m_open)
        return 0;
    QSqlDatabase db = QSqlDatabase::database(m_conn);
    if (!db.transaction())
        return 0;
    int added = 0;
    QSqlQuery ins(db);
    // OR IGNORE (not check-then-insert): one round trip per row instead
    // of two, same skip-duplicates semantics via changes().
    ins.prepare(QStringLiteral(
        "INSERT OR IGNORE INTO favourites (site, post_id, rating, score, ext, tags, "
        "tags_artist, tags_character, tags_copyright, tags_general, tags_meta, "
        "file_url, sample_url, preview_url, post_url, width, height, added, untyped, custom) "
        "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"));
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (const Post &post : posts) {
        const QString site = FavDb::siteFor(post);
        const qint64 pid = FavDb::pidFor(post);
        ins.bindValue(0, site);
        ins.bindValue(1, pid);
        ins.bindValue(2, post.rating);
        ins.bindValue(3, post.score);
        ins.bindValue(4, post.ext);
        ins.bindValue(5, post.tags);
        ins.bindValue(6, post.tagsArtist);
        ins.bindValue(7, post.tagsCharacter);
        ins.bindValue(8, post.tagsCopyright);
        ins.bindValue(9, post.tagsGeneral);
        ins.bindValue(10, post.tagsMeta);
        ins.bindValue(11, post.fileUrl);
        ins.bindValue(12, post.sampleUrl);
        ins.bindValue(13, post.previewUrl);
        ins.bindValue(14, post.isRemote
                            ? (post.postUrl.isEmpty() ? post.absFile : post.postUrl)
                            : post.absFile);
        ins.bindValue(15, post.width);
        ins.bindValue(16, post.height);
        ins.bindValue(17, now);
        ins.bindValue(18, post.untyped);
        ins.bindValue(19, encodeCustom(post.customTags));
        if (ins.exec() && ins.numRowsAffected() > 0)
            added++;
    }
    if (!db.commit()) {
        db.rollback();
        return 0;
    }
    return added;
}

bool FavDb::remove(const QString &site, qint64 postId) {
    if (!m_open)
        return false;
    QSqlQuery q(QSqlDatabase::database(m_conn));
    q.prepare(QStringLiteral("DELETE FROM favourites WHERE site=? AND post_id=?"));
    q.addBindValue(site);
    q.addBindValue(postId);
    return q.exec();
}

int FavDb::count() {
    if (!m_open)
        return 0;
    QSqlQuery q(QSqlDatabase::database(m_conn));
    if (!q.exec(QStringLiteral("SELECT COUNT(*) FROM favourites")) || !q.next())
        return 0;
    return q.value(0).toInt();
}

static Post rowToPost(const QSqlQuery &q) {
    Post p;
    const QString site = q.value(0).toString();
    p.isRemote = site != QStringLiteral("local");
    // Keep the site label even when a downloaded copy takes over below:
    // identity (favIdentity), badges, and writebacks all key on it.
    p.siteName = site == QStringLiteral("local") ? QString() : site;
    p.id = q.value(1).toLongLong();
    p.rating = q.value(2).toString();
    p.score = q.value(3).toInt();
    p.ext = q.value(4).toString();
    p.tags = q.value(5).toString();
    p.tagsArtist = q.value(6).toString();
    p.tagsCharacter = q.value(7).toString();
    p.tagsCopyright = q.value(8).toString();
    p.tagsGeneral = q.value(9).toString();
    p.tagsMeta = q.value(10).toString();
    p.fileUrl = q.value(11).toString();
    p.sampleUrl = q.value(12).toString();
    p.previewUrl = q.value(13).toString();
    const QString url = q.value(14).toString();
    if (p.isRemote)
        p.postUrl = url;
    else
        p.absFile = url;
    p.file = p.isRemote ? QString() : url;
    p.width = q.value(15).toInt();
    p.height = q.value(16).toInt();
    p.untyped = q.value(18).toString();
    p.customTags = decodeCustom(q.value(19).toString());
    p.hasMeta = true;
    // LOSN imports (and old rows) store "general" with no rating source:
    // upgrade in memory when the tags hit the explicit list, same rule
    // the scanner applies at parse time (metaparser). list() persists
    // the upgrade below so the DB heals once instead of every listing.
    {
        const auto [rating, inferred] =
            inferExplicit(p.tags.split(QChar::Space, Qt::SkipEmptyParts),
                          normalizeRating(p.rating));
        p.rating = rating;
        p.ratingInferred = inferred;
    }
    p.category = p.isRemote ? p.siteName : QStringLiteral("Favourites");
    // A downloaded copy always wins: favourites open (and thumbnail) from
    // disk instead of re-streaming the remote URL.
    const QString local = q.value(17).toString();
    if (!local.isEmpty() && QFileInfo::exists(local)) {
        p.isRemote = false;
        p.absFile = local;
        p.file = local;
        p.ext = QFileInfo(local).suffix().toLower();
        if (!p.siteName.isEmpty())
            p.category = p.siteName;
    }
    return p;
}

QVector<Post> FavDb::list(int limit) {
    QVector<Post> out;
    if (!m_open)
        return out;
    QSqlQuery q(QSqlDatabase::database(m_conn));
    q.prepare(QStringLiteral(
        "SELECT site, post_id, rating, score, ext, tags, tags_artist, "
        "tags_character, tags_copyright, tags_general, tags_meta, file_url, "
        "sample_url, preview_url, post_url, width, height, local_path, untyped, custom "
        "FROM favourites "
        "ORDER BY added DESC, rowid DESC LIMIT ?")); // rowid breaks same-ms ties
    q.addBindValue(limit);
    if (!q.exec())
        return out;
    QHash<QPair<QString, qint64>, QString> heal;
    while (q.next()) {
        const QString storedSite = q.value(0).toString();
        const qint64 storedPid = q.value(1).toLongLong();
        Post p = rowToPost(q);
        // Persist explicit upgrades (see rowToPost): the stored value
        // feeds rating counts/filters, so heal it once.
        if (p.ratingInferred)
            heal.insert({storedSite, storedPid}, p.rating);
        out.append(p);
    }
    if (!heal.isEmpty()) {
        QSqlDatabase db = QSqlDatabase::database(m_conn);
        if (db.transaction()) {
            QSqlQuery u(db);
            u.prepare(QStringLiteral(
                "UPDATE favourites SET rating=? WHERE site=? AND post_id=?"));
            for (auto it = heal.begin(); it != heal.end(); ++it) {
                u.addBindValue(it.value());
                u.addBindValue(it.key().first);
                u.addBindValue(it.key().second);
                u.exec();
            }
            if (!db.commit())
                db.rollback();
        }
    }
    return out;
}

void FavDb::clear() {
    if (!m_open)
        return;
    QSqlQuery q(QSqlDatabase::database(m_conn));
    q.exec(QStringLiteral("DELETE FROM favourites"));
}
