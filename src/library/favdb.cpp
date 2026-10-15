#include "favdb.h"
#include "settings.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>

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
                               "added INTEGER, local_path TEXT, untyped TEXT, PRIMARY KEY (site, post_id))"))) {
        close();
        return false;
    }
    QSqlQuery alter(db);
    alter.exec(QStringLiteral(
        "ALTER TABLE favourites ADD COLUMN local_path TEXT"));
    alter.exec(QStringLiteral(
        "ALTER TABLE favourites ADD COLUMN untyped TEXT"));
    Q_UNUSED(alter); // fails when the columns already exist: fine
    // Backfill: rows predating the column get their current general tags
    // as untyped. Correct in both cases: never-classified rows keep the
    // whole blob, and classified rows already moved resolved tags out of
    // general, leaving exactly the still-untyped remainder.
    QSqlQuery backfill(db);
    backfill.exec(QStringLiteral(
        "UPDATE favourites SET untyped=tags_general WHERE untyped IS NULL"));
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
        "file_url, sample_url, preview_url, post_url, width, height, added, untyped) "
        "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"));
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
    return q.exec();
}

int FavDb::addMany(const QVector<Post> &posts) {
    if (!m_open)
        return 0;
    QSqlDatabase db = QSqlDatabase::database(m_conn);
    if (!db.transaction())
        return 0;
    int added = 0;
    QSqlQuery check(db);
    check.prepare(QStringLiteral("SELECT 1 FROM favourites WHERE site=? AND post_id=?"));
    QSqlQuery ins(db);
    ins.prepare(QStringLiteral(
        "INSERT INTO favourites (site, post_id, rating, score, ext, tags, "
        "tags_artist, tags_character, tags_copyright, tags_general, tags_meta, "
        "file_url, sample_url, preview_url, post_url, width, height, added, untyped) "
        "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"));
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (const Post &post : posts) {
        const QString site = FavDb::siteFor(post);
        const qint64 pid = FavDb::pidFor(post);
        check.bindValue(0, site);
        check.bindValue(1, pid);
        if (!check.exec() || check.next())
            continue; // duplicate (or transient error): skip
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
        if (ins.exec())
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
    p.hasMeta = true;
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
        "sample_url, preview_url, post_url, width, height, local_path, untyped "
        "FROM favourites "
        "ORDER BY added DESC, rowid DESC LIMIT ?")); // rowid breaks same-ms ties
    q.addBindValue(limit);
    if (!q.exec())
        return out;
    while (q.next())
        out.append(rowToPost(q));
    return out;
}

void FavDb::clear() {
    if (!m_open)
        return;
    QSqlQuery q(QSqlDatabase::database(m_conn));
    q.exec(QStringLiteral("DELETE FROM favourites"));
}
