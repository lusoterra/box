#include "favdb.h"
#include "explicittags.h"
#include "metaparser.h"
#include "migrations/migrations.h"
#include "settings.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QCryptographicHash>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUrl>

#include <cstring>
#include <algorithm>

namespace {
// Scheme check without pulling the engine layer into favdb: only http(s)
// values are post pages; anything else in the post_url column is a
// backing file (strays) or legacy data and must never surface as the
// post link.
bool isHttpLike(const QString &s) {
    const QString head = s.left(8).toLower();
    return head.startsWith(QStringLiteral("http://")) ||
           head.startsWith(QStringLiteral("https://"));
}

bool mediaSuffix(const QString &suffix) {
    static const QStringList exts = {
        QStringLiteral("png"),  QStringLiteral("jpg"),
        QStringLiteral("jpeg"), QStringLiteral("gif"),
        QStringLiteral("webp"), QStringLiteral("avif"),
        QStringLiteral("jxl"),  QStringLiteral("mp4"),
        QStringLiteral("webm"), QStringLiteral("bmp"),
        QStringLiteral("tif"),  QStringLiteral("tiff")};
    return exts.contains(suffix.toLower());
}

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

bool FavDb::isSidecarPath(const QString &path) {
    return QFileInfo(path).suffix().compare(QStringLiteral("json"),
                                            Qt::CaseInsensitive) == 0;
}

QString FavDb::normalizedPostUrl(const QString &raw) {
    if (!isHttpLike(raw))
        return {};
    const QUrl u(raw.trimmed());
    if (!u.isValid() || u.scheme().isEmpty() || u.host().isEmpty())
        return {};
    QString path = u.path();
    while (path.endsWith(u'/') && path.size() > 1)
        path.chop(1);
    QString out = u.scheme().toLower() + QStringLiteral("://") +
                  u.host().toLower();
    if (u.port() > 0)
        out += u'_' + QString::number(u.port());
    out += path;
    const QString query = u.query(QUrl::FullyEncoded);
    if (!query.isEmpty())
        out += u'?' + query;
    return out;
}

QString FavDb::mediaPathForSidecar(const QString &path) {
    const QFileInfo fi(path);
    if (fi.suffix().compare(QStringLiteral("json"), Qt::CaseInsensitive) != 0)
        return {};
    // gallery-dl flat sidecar: image.jpg.json -> image.jpg
    const QString flat = path.left(path.size() - 5);
    if (!flat.isEmpty() && QFileInfo::exists(flat) &&
        !isSidecarPath(flat))
        return flat;
    // Our stem sidecar: Site_md5.json -> Site_md5.<media ext>. Directory
    // listing keeps case-insensitive extensions working on Linux.
    const QString dir = fi.absolutePath();
    const QString stem = fi.completeBaseName();
    const QFileInfoList files =
        QDir(dir).entryInfoList(QDir::Files | QDir::NoDotAndDotDot);
    for (const QFileInfo &cand : files) {
        if (cand.completeBaseName() == stem &&
            mediaSuffix(cand.suffix()) && !isSidecarPath(cand.fileName()))
            return cand.absoluteFilePath();
    }
    return {};
}

qint64 FavDb::pidFor(const Post &post) {
    if (post.siteName.isEmpty() && !post.isRemote)
        return stableId(post.absFile);
    if (post.id != 0)
        return post.id;
    return stableId(post.fileUrl.isEmpty() ? post.absFile : post.fileUrl);
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
    // One-time schema upgrades live in src/migrations/ (m_favdb_schema,
    // m_favdb_hash_ids): the boot runner performs them with progress, and
    // open() keeps the same idempotent helpers for temp DBs and data dirs
    // the runner never saw.
    if (!Migrations::ensureFavDbSchema(db)) {
        close();
        return false;
    }
    // Safety net: heal only when a legacy (site,0) row is still present
    // (indexed probe, microseconds when clean).
    {
        QSqlQuery probe(db);
        if (probe.exec(QStringLiteral(
                "SELECT 1 FROM favourites WHERE post_id=0 LIMIT 1")) &&
            probe.next())
            Migrations::healFavDbHashIds(db);
    }
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
        "UPDATE favourites SET tags=?, tags_artist=?, tags_character=?, "
        "tags_copyright=?, tags_general=?, tags_meta=?, untyped=? "
        "WHERE site=? AND post_id=?"));
    for (auto it = splits.begin(); it != splits.end(); ++it) {
        const TagSplit &s = it.value();
        q.bindValue(0, s.tags);
        q.bindValue(1, s.artist);
        q.bindValue(2, s.character);
        q.bindValue(3, s.copyright);
        q.bindValue(4, s.general);
        q.bindValue(5, s.meta);
        q.bindValue(6, s.untyped);
        q.bindValue(7, it.key().first);
        q.bindValue(8, it.key().second);
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
                               "tags, ext, post_url FROM favourites")))
        return out;
    while (q.next()) {
        const QString local = q.value(3).toString();
        // Sidecar links are always re-resolved: even when the JSON exists,
        // the row must point at the sibling media instead.
        if (!local.isEmpty() && !isSidecarPath(local) &&
            QFileInfo::exists(local))
            continue;
        Unlinked u;
        u.site = q.value(0).toString();
        u.id = q.value(1).toLongLong();
        u.fileUrl = q.value(2).toString();
        u.tags = q.value(4).toString();
        u.ext = q.value(5).toString();
        u.postUrl = q.value(6).toString();
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

QHash<QPair<QString, qint64>, QString> FavDb::localPaths() {
    QHash<QPair<QString, qint64>, QString> out;
    if (!m_open)
        return out;
    QSqlQuery q(QSqlDatabase::database(m_conn));
    if (!q.exec(QStringLiteral("SELECT site, post_id, local_path FROM favourites")))
        return out;
    while (q.next()) {
        QString local = q.value(2).toString();
        // Broken older link: a sidecar was chosen over its media sibling
        // (QDir ordered image.json before image.png). Heal in memory; the
        // startup relink pass persists the media path. A sidecar without
        // media maps to empty, so the row is not treated as downloaded.
        if (isSidecarPath(local))
            local = mediaPathForSidecar(local);
        out.insert({q.value(0).toString(), q.value(1).toLongLong()}, local);
    }
    return out;
}

QHash<QPair<QString, qint64>, QString> FavDb::mediaPaths() {
    QHash<QPair<QString, qint64>, QString> out;
    if (!m_open)
        return out;
    QSqlQuery q(QSqlDatabase::database(m_conn));
    if (!q.exec(QStringLiteral(
            "SELECT site, post_id, local_path, post_url FROM favourites")))
        return out;
    while (q.next()) {
        QString local = q.value(2).toString();
        // Strays favourited as remote rows can carry their backing file in
        // the post_url column with no local_path. They still must receive
        // tag edits written from the scanner/Downloads row.
        if (local.isEmpty()) {
            const QString backing = q.value(3).toString();
            if (!backing.isEmpty() && !isHttpLike(backing))
                local = backing;
        }
        if (isSidecarPath(local))
            local = mediaPathForSidecar(local);
        out.insert({q.value(0).toString(), q.value(1).toLongLong()}, local);
    }
    return out;
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
        "file_url, sample_url, preview_url, post_url, width, height, added, untyped, custom, local_path) "
        "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"));
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
    // Local posts keep their file path for rowToPost and, when the sidecar
    // recorded a page, the real post URL too: Favourites can then collapse
    // the remote row and this local one by URL (and Open post keeps working).
    q.addBindValue(post.isRemote
                       ? (post.postUrl.isEmpty() ? post.absFile : post.postUrl)
                       : (isHttpLike(post.postUrl) ? post.postUrl
                                                   : post.absFile));
    q.addBindValue(post.width);
    q.addBindValue(post.height);
    q.addBindValue(QDateTime::currentMSecsSinceEpoch());
    q.addBindValue(post.untyped);
    q.addBindValue(encodeCustom(post.customTags));
    q.addBindValue(post.isRemote ? QString() : post.absFile);
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
        "file_url, sample_url, preview_url, post_url, width, height, added, untyped, custom, local_path) "
        "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"));
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
                            : (isHttpLike(post.postUrl) ? post.postUrl
                                                        : post.absFile));
        ins.bindValue(15, post.width);
        ins.bindValue(16, post.height);
        // Restores preserve the original add time; fresh rows stamp now.
        ins.bindValue(17, post.added > 0 ? post.added : now);
        ins.bindValue(18, post.untyped);
        ins.bindValue(19, encodeCustom(post.customTags));
        ins.bindValue(20, post.isRemote ? QString() : post.absFile);
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
    // Report whether a row actually died: callers keying a local post by
    // ("local", file-hash) otherwise believe they removed its ("Site",
    // id) row when nothing happened.
    return q.exec() && q.numRowsAffected() > 0;
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
    p.ext = q.value(4).toString().toLower();
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
    if (p.isRemote) {
        // The post page survives a download linking the row to disk (the
        // local_path override below flips isRemote but keeps postUrl):
        // only http(s) values are post links; strays carry a backing
        // file here instead and must keep pointing at it.
        if (isHttpLike(url))
            p.postUrl = url;
        else if (!url.isEmpty())
            p.absFile = url;
    } else {
        // New rows store the real page here and the file in local_path;
        // older rows stored the file path itself.
        if (isHttpLike(url))
            p.postUrl = url;
        else
            p.absFile = url;
    }
    p.file = p.isRemote ? QString() : p.absFile;
    p.width = q.value(15).toInt();
    p.height = q.value(16).toInt();
    p.untyped = q.value(18).toString();
    p.customTags = decodeCustom(q.value(19).toString());
    // Older LOSN imports stored one flat blob in tags_general with no
    // untyped list, so lucky_star stayed General while downloaded siblings
    // showed Copyright (4 vs 200 split). A flat favourite (no other
    // buckets) is category-less by definition: offer it to the cached
    // classifier once, which persists the healed splits.
    if (p.isRemote && p.untyped.isEmpty() && !p.tagsGeneral.isEmpty() &&
        p.tagsArtist.isEmpty() && p.tagsCharacter.isEmpty() &&
        p.tagsCopyright.isEmpty() && p.tagsMeta.isEmpty() &&
        p.customTags.isEmpty() && p.siteName != QStringLiteral("_Stray"))
        p.untyped = p.tagsGeneral;
    // Heal rows where a tag leaked into two buckets (old duplicate bug):
    // keep the most specific bucket so lucky_star is not bold in both
    // Copyright and Meta, and counts stop splitting.
    {
        QStringList g = p.tagsGeneral.split(QChar::Space, Qt::SkipEmptyParts);
        QStringList a = p.tagsArtist.split(QChar::Space, Qt::SkipEmptyParts);
        QStringList c = p.tagsCharacter.split(QChar::Space, Qt::SkipEmptyParts);
        QStringList cr = p.tagsCopyright.split(QChar::Space, Qt::SkipEmptyParts);
        QStringList m = p.tagsMeta.split(QChar::Space, Qt::SkipEmptyParts);
        const QList<QPair<int, QStringList *>> prio = {
            {4, &a}, {3, &c}, {2, &cr}, {1, &m}, {0, &g}};
        QHash<QString, int> bucketCount;
        for (const auto &[pr, list] : prio)
            for (const QString &t : *list)
                bucketCount[t]++;
        for (auto it = bucketCount.begin(); it != bucketCount.end(); ++it) {
            if (it.value() < 2)
                continue;
            int best = -1;
            for (const auto &[pr, list] : prio)
                if (list->contains(it.key()) && pr > best)
                    best = pr;
            for (const auto &[pr, list] : prio)
                if (pr != best)
                    list->removeAll(it.key());
        }
        p.tagsGeneral = g.join(u' ');
        p.tagsArtist = a.join(u' ');
        p.tagsCharacter = c.join(u' ');
        p.tagsCopyright = cr.join(u' ');
        p.tagsMeta = m.join(u' ');
    }
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
    if (p.isRemote)
        p.category = p.siteName;
    else if (!p.siteName.isEmpty())
        p.category = p.siteName;
    else {
        // Local favourite with no recorded source: derive one from the
        // on-disk path/filename so the source filter can group it like
        // downloads instead of lumping everything under "Favourites".
        const QFileInfo fi(p.absFile);
        const QStringList parts = fi.absolutePath().split(u'/', Qt::SkipEmptyParts);
        // Drop the downloads root's last? Use first meaningful parent like scanner.
        if (!parts.isEmpty() && parts.last() != QStringLiteral("Favourites"))
            p.category = parts.last();
        else {
            const QString stem = fi.completeBaseName();
            const qsizetype us = stem.indexOf(u'_');
            p.category = (us > 0) ? stem.left(us) : QStringLiteral("Favourites");
        }
        if (p.category.isEmpty())
            p.category = QStringLiteral("Favourites");
    }
    // Old/imported rows sometimes lack ext (empty string): derive it from
    // the file URL or path so media routing (video vs image), grid badges
    // and download naming keep working without a DB migration.
    if (p.ext.isEmpty()) {
        QString src = !p.fileUrl.isEmpty() ? p.fileUrl : p.absFile;
        const qsizetype qi = src.indexOf(u'?');
        if (qi >= 0)
            src = src.left(qi);
        QString suf = QFileInfo(src).suffix().toLower();
        bool ok = !suf.isEmpty() && suf.size() <= 5;
        for (const QChar &c : suf) {
            if (!c.isLetterOrNumber()) {
                ok = false;
                break;
            }
        }
        if (ok)
            p.ext = suf;
    }
    // A downloaded copy always wins: favourites open (and thumbnail) from
    // disk instead of re-streaming the remote URL.
    QString local = q.value(17).toString();
    if (!local.isEmpty() && FavDb::isSidecarPath(local)) {
        // Heal the old relink bug: `Site_md5.json` won over `Site_md5.png`
        // because both share the stem. Never serve the sidecar as media.
        local = FavDb::mediaPathForSidecar(local);
    }
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
    // Rows whose tag columns are empty (old write-back misses) heal from
    // their local sidecar once and persist, so Favourites stops showing a
    // blank post while the JSON clearly has the hand-written tags.
    QHash<QPair<QString, qint64>, FavDb::TagSplit> tagHeal;
    // Canonical local path and canonical post page both collapse twins:
    // the remote favourite may never have been linked to its downloaded
    // copy, so a path-only rule still showed "the same post twice".
    QHash<QString, int> byPath;
    QHash<QString, int> byPost;
    QHash<int, bool> localRow;
    auto tagSet = [](const QString &s) {
        const QStringList parts = s.split(u' ', Qt::SkipEmptyParts);
        return QSet<QString>(parts.begin(), parts.end());
    };
    while (q.next()) {
        const QString storedSite = q.value(0).toString();
        const qint64 storedPid = q.value(1).toLongLong();
        const bool isLocalRow = storedSite == QStringLiteral("local");
        const QString localPath = q.value(17).toString();
        QString canon;
        if (!localPath.isEmpty()) {
            canon = QFileInfo(localPath).canonicalFilePath();
            if (canon.isEmpty())
                canon = QFileInfo(localPath).absoluteFilePath();
        }
        Post p = rowToPost(q);
        if (p.tags.trimmed().isEmpty() && !p.absFile.isEmpty()) {
            const QString sidecar = localSidecarPath(p.absFile);
            if (!sidecar.isEmpty()) {
                QFile f(sidecar);
                if (f.open(QIODevice::ReadOnly)) {
                    QJsonParseError err;
                    const QJsonDocument doc =
                        QJsonDocument::fromJson(f.readAll(), &err);
                    Post parsed;
                    if (err.error == QJsonParseError::NoError &&
                        doc.isObject() &&
                        MetaParser::parse(doc.object(), parsed) &&
                        !parsed.tags.trimmed().isEmpty()) {
                        p.tags = parsed.tags;
                        p.tagsArtist = parsed.tagsArtist;
                        p.tagsCharacter = parsed.tagsCharacter;
                        p.tagsCopyright = parsed.tagsCopyright;
                        p.tagsGeneral = parsed.tagsGeneral;
                        p.tagsMeta = parsed.tagsMeta;
                        p.customTags = parsed.customTags;
                        p.untyped = parsed.untyped;
                        if (!parsed.rating.isEmpty())
                            p.rating = parsed.rating;
                        FavDb::TagSplit split;
                        split.tags = p.tags;
                        split.artist = p.tagsArtist;
                        split.character = p.tagsCharacter;
                        split.copyright = p.tagsCopyright;
                        split.general = p.tagsGeneral;
                        split.meta = p.tagsMeta;
                        split.untyped = p.untyped;
                        tagHeal.insert({storedSite, storedPid}, split);
                    }
                }
            }
        }
        // Persist explicit upgrades (see rowToPost): the stored value
        // feeds rating counts/filters, so heal it once.
        if (p.ratingInferred)
            heal.insert({storedSite, storedPid}, p.rating);
        const QString postKey = FavDb::normalizedPostUrl(p.postUrl);
        int hit = -1;
        if (!canon.isEmpty()) {
            const auto it = byPath.constFind(canon);
            if (it != byPath.constEnd())
                hit = it.value();
        }
        if (hit < 0 && !postKey.isEmpty()) {
            const auto it = byPost.constFind(postKey);
            // Only collapse a remote row with its local/hash twin (or two
            // local rows for one file). Two distinct remote rows that
            // happen to share a page are left alone.
            if (it != byPost.constEnd() &&
                (isLocalRow || localRow.value(it.value())))
                hit = it.value();
        }
        if (hit < 0) {
            hit = out.size();
            out.append(p);
        } else {
            // Keep the newest row's position, but never lose the richer
            // side: the remote row usually carries the full tag set and
            // the page link, while the local row carries the file.
            Post &kept = out[hit];
            if (!isHttpLike(kept.postUrl) && isHttpLike(p.postUrl))
                kept.postUrl = p.postUrl;
            const QSet<QString> have = tagSet(kept.tags);
            const QSet<QString> other = tagSet(p.tags);
            if (other.size() > have.size() &&
                std::all_of(have.begin(), have.end(),
                            [&](const QString &t) { return other.contains(t); })) {
                kept.tags = p.tags;
                kept.tagsArtist = p.tagsArtist;
                kept.tagsCharacter = p.tagsCharacter;
                kept.tagsCopyright = p.tagsCopyright;
                kept.tagsGeneral = p.tagsGeneral;
                kept.tagsMeta = p.tagsMeta;
                kept.untyped = p.untyped;
                kept.customTags = p.customTags;
                kept.rating = p.rating;
                kept.ratingInferred = p.ratingInferred;
            }
            if (kept.absFile.isEmpty() && !p.absFile.isEmpty()) {
                kept.absFile = p.absFile;
                kept.file = p.file;
                kept.ext = p.ext;
            }
        }
        if (!canon.isEmpty())
            byPath.insert(canon, hit);
        if (!postKey.isEmpty())
            byPost.insert(postKey, hit);
        localRow[hit] = localRow.value(hit) || isLocalRow;
    }
    // Release the read statement before the heal writes start a
    // transaction on the same connection (SQLite otherwise sees an
    // active statement on some builds).
    q.finish();
    if (!tagHeal.isEmpty())
        setTagSplits(tagHeal);
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

QHash<QPair<QString, qint64>, qint64> FavDb::addedTimes() {
    QHash<QPair<QString, qint64>, qint64> out;
    if (!m_open)
        return out;
    QSqlQuery q(QSqlDatabase::database(m_conn));
    if (!q.exec(QStringLiteral("SELECT site, post_id, added FROM favourites")))
        return out;
    while (q.next())
        out.insert({q.value(0).toString(), q.value(1).toLongLong()},
                   q.value(2).toLongLong());
    return out;
}

void FavDb::clear() {
    if (!m_open)
        return;
    QSqlQuery q(QSqlDatabase::database(m_conn));
    q.exec(QStringLiteral("DELETE FROM favourites"));
}
