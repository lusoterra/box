#include "downloadqueue.h"

#include "downloadmanager.h"

#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>

namespace {
QJsonArray toJson(const QStringList &list) {
    QJsonArray out;
    for (const QString &s : list)
        out.append(s);
    return out;
}
QStringList fromJson(const QJsonValue &v) {
    QStringList out;
    for (const QJsonValue &e : v.toArray()) {
        const QString s = e.toString();
        if (!s.isEmpty())
            out << s;
    }
    return out;
}

QJsonObject postToJson(const Post &p) {
    QJsonObject o;
    o[QStringLiteral("file")] = p.file;
    o[QStringLiteral("abs_file")] = p.absFile;
    o[QStringLiteral("ext")] = p.ext;
    o[QStringLiteral("id")] = QString::number(p.id);
    o[QStringLiteral("score")] = p.score;
    o[QStringLiteral("rating")] = p.rating;
    o[QStringLiteral("rating_inferred")] = p.ratingInferred;
    o[QStringLiteral("tags")] = p.tags;
    o[QStringLiteral("tags_artist")] = p.tagsArtist;
    o[QStringLiteral("tags_character")] = p.tagsCharacter;
    o[QStringLiteral("tags_copyright")] = p.tagsCopyright;
    o[QStringLiteral("tags_meta")] = p.tagsMeta;
    o[QStringLiteral("tags_general")] = p.tagsGeneral;
    o[QStringLiteral("untyped")] = p.untyped;
    o[QStringLiteral("width")] = p.width;
    o[QStringLiteral("height")] = p.height;
    o[QStringLiteral("source")] = p.source;
    o[QStringLiteral("date")] = p.date;
    o[QStringLiteral("title")] = p.title;
    o[QStringLiteral("post_url")] = p.postUrl;
    o[QStringLiteral("category")] = p.category;
    o[QStringLiteral("has_meta")] = p.hasMeta;
    o[QStringLiteral("comment_count")] = p.commentCount;
    o[QStringLiteral("has_comments")] = p.hasComments;
    o[QStringLiteral("has_notes")] = p.hasNotes;
    o[QStringLiteral("is_remote")] = p.isRemote;
    o[QStringLiteral("site_name")] = p.siteName;
    o[QStringLiteral("site_key")] = p.siteKey;
    o[QStringLiteral("site_short")] = p.siteShort;
    o[QStringLiteral("file_url")] = p.fileUrl;
    o[QStringLiteral("sample_url")] = p.sampleUrl;
    o[QStringLiteral("preview_url")] = p.previewUrl;
    o[QStringLiteral("md5")] = p.md5;
    o[QStringLiteral("raw")] = p.rawJson;
    QJsonObject custom;
    for (auto it = p.customTags.begin(); it != p.customTags.end(); ++it)
        custom[it.key()] = toJson(it.value());
    o[QStringLiteral("custom_tags")] = custom;
    return o;
}

Post postFromJson(const QJsonObject &o) {
    Post p;
    p.file = o.value(QStringLiteral("file")).toString();
    p.absFile = o.value(QStringLiteral("abs_file")).toString();
    p.ext = o.value(QStringLiteral("ext")).toString();
    p.id = o.value(QStringLiteral("id")).toString().toLongLong();
    p.score = o.value(QStringLiteral("score")).toInt();
    p.rating = o.value(QStringLiteral("rating")).toString();
    p.ratingInferred = o.value(QStringLiteral("rating_inferred")).toBool();
    p.tags = o.value(QStringLiteral("tags")).toString();
    p.tagsArtist = o.value(QStringLiteral("tags_artist")).toString();
    p.tagsCharacter = o.value(QStringLiteral("tags_character")).toString();
    p.tagsCopyright = o.value(QStringLiteral("tags_copyright")).toString();
    p.tagsMeta = o.value(QStringLiteral("tags_meta")).toString();
    p.tagsGeneral = o.value(QStringLiteral("tags_general")).toString();
    p.untyped = o.value(QStringLiteral("untyped")).toString();
    p.width = o.value(QStringLiteral("width")).toInt();
    p.height = o.value(QStringLiteral("height")).toInt();
    p.source = o.value(QStringLiteral("source")).toString();
    p.date = o.value(QStringLiteral("date")).toString();
    p.title = o.value(QStringLiteral("title")).toString();
    p.postUrl = o.value(QStringLiteral("post_url")).toString();
    p.category = o.value(QStringLiteral("category")).toString();
    p.hasMeta = o.value(QStringLiteral("has_meta")).toBool();
    p.commentCount = o.value(QStringLiteral("comment_count")).toInt();
    p.hasComments = o.value(QStringLiteral("has_comments")).toBool();
    p.hasNotes = o.value(QStringLiteral("has_notes")).toBool();
    p.isRemote = o.value(QStringLiteral("is_remote")).toBool();
    p.siteName = o.value(QStringLiteral("site_name")).toString();
    p.siteKey = o.value(QStringLiteral("site_key")).toString();
    p.siteShort = o.value(QStringLiteral("site_short")).toString();
    p.fileUrl = o.value(QStringLiteral("file_url")).toString();
    p.sampleUrl = o.value(QStringLiteral("sample_url")).toString();
    p.previewUrl = o.value(QStringLiteral("preview_url")).toString();
    p.md5 = o.value(QStringLiteral("md5")).toString();
    p.rawJson = o.value(QStringLiteral("raw")).toObject();
    const QJsonObject custom = o.value(QStringLiteral("custom_tags")).toObject();
    for (auto it = custom.begin(); it != custom.end(); ++it)
        p.customTags.insert(it.key(), fromJson(it.value()));
    return p;
}
} // namespace

DownloadQueueStore::DownloadQueueStore(const QString &dbPath) : m_path(dbPath) {
    if (!m_path.isEmpty())
        open();
}

DownloadQueueStore::~DownloadQueueStore() {
    close();
}

bool DownloadQueueStore::open() {
    close();
    if (m_path.isEmpty())
        return false;
    QDir().mkpath(QFileInfo(m_path).absolutePath());
    m_conn = QStringLiteral("download_queue_%1").arg(
        QUuid::createUuid().toString(QUuid::WithoutBraces));
    QSqlDatabase db =
        QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_conn);
    db.setDatabaseName(m_path);
    if (!db.open()) {
        qWarning() << "download queue: cannot open" << m_path << db.lastError();
        db = QSqlDatabase();
        close();
        return false;
    }
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS jobs ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "url TEXT NOT NULL UNIQUE,"
            "post TEXT NOT NULL,"
            "file_name TEXT NOT NULL,"
            "file_path TEXT NOT NULL,"
            "json_path TEXT NOT NULL)"))) {
        qWarning() << "download queue: cannot create schema" << q.lastError();
        q = QSqlQuery();
        db = QSqlDatabase();
        close();
        return false;
    }
    return true;
}

void DownloadQueueStore::close() {
    if (m_conn.isEmpty())
        return;
    const QString conn = m_conn;
    m_conn.clear();
    {
        QSqlDatabase db = QSqlDatabase::database(conn, false);
        if (db.isOpen())
            db.close();
    }
    QSqlDatabase::removeDatabase(conn);
}

bool DownloadQueueStore::add(const QList<DownloadJob> &jobs) {
    if (!isOpen() || jobs.isEmpty())
        return false;
    QSqlDatabase db = QSqlDatabase::database(m_conn);
    if (!db.transaction())
        return false;
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO jobs(url, post, file_name, file_path, "
        "json_path) VALUES (?, ?, ?, ?, ?)"));
    for (const DownloadJob &job : jobs) {
        if (job.post.fileUrl.isEmpty() || job.filePath.isEmpty())
            continue;
        q.bindValue(0, job.post.fileUrl);
        q.bindValue(1, QString::fromUtf8(
                           QJsonDocument(jobToJson(job))
                               .toJson(QJsonDocument::Compact)));
        q.bindValue(2, job.fileName);
        q.bindValue(3, job.filePath);
        q.bindValue(4, job.jsonPath);
        if (!q.exec()) {
            qWarning() << "download queue: insert failed" << q.lastError();
            db.rollback();
            return false;
        }
    }
    return db.commit();
}

bool DownloadQueueStore::removeUrl(const QString &url) {
    if (!isOpen() || url.isEmpty())
        return false;
    QSqlQuery q(QSqlDatabase::database(m_conn));
    q.prepare(QStringLiteral("DELETE FROM jobs WHERE url=?"));
    q.addBindValue(url);
    return q.exec();
}

bool DownloadQueueStore::clear() {
    if (!isOpen())
        return false;
    QSqlQuery q(QSqlDatabase::database(m_conn));
    return q.exec(QStringLiteral("DELETE FROM jobs"));
}

QList<DownloadJob> DownloadQueueStore::load() const {
    QList<DownloadJob> out;
    if (!isOpen())
        return out;
    QSqlQuery q(QSqlDatabase::database(m_conn));
    if (!q.exec(QStringLiteral(
            "SELECT post, file_name, file_path, json_path FROM jobs "
            "ORDER BY id"))) {
        return out;
    }
    while (q.next()) {
        DownloadJob job;
        const QJsonObject o = QJsonDocument::fromJson(q.value(0).toByteArray())
                                  .object();
        if (!jobFromJson(o, job))
            continue;
        job.fileName = q.value(1).toString();
        job.filePath = q.value(2).toString();
        job.jsonPath = q.value(3).toString();
        if (job.filePath.isEmpty() || job.jsonPath.isEmpty())
            continue;
        out.append(job);
    }
    return out;
}

int DownloadQueueStore::count() const {
    if (!isOpen())
        return 0;
    QSqlQuery q(QSqlDatabase::database(m_conn));
    if (q.exec(QStringLiteral("SELECT COUNT(*) FROM jobs")) && q.next())
        return q.value(0).toInt();
    return 0;
}

QJsonObject DownloadQueueStore::jobToJson(const DownloadJob &job) {
    QJsonObject o;
    o[QStringLiteral("post")] = postToJson(job.post);
    o[QStringLiteral("file_name")] = job.fileName;
    o[QStringLiteral("file_path")] = job.filePath;
    o[QStringLiteral("json_path")] = job.jsonPath;
    return o;
}

bool DownloadQueueStore::jobFromJson(const QJsonObject &o, DownloadJob &job) {
    const QJsonObject postObj = o.value(QStringLiteral("post")).toObject();
    if (postObj.isEmpty())
        return false;
    job.post = postFromJson(postObj);
    job.fileName = o.value(QStringLiteral("file_name")).toString();
    job.filePath = o.value(QStringLiteral("file_path")).toString();
    job.jsonPath = o.value(QStringLiteral("json_path")).toString();
    return job.post.isRemote && !job.post.fileUrl.isEmpty() &&
           !job.filePath.isEmpty();
}
