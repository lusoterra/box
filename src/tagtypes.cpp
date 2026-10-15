#include "tagtypes.h"

#include "remotesource.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTimer>
#include <QUrl>
#include <QXmlStreamReader>

namespace TagTypes {

QString bucketForType(int type) {
    switch (type) {
    case 1: return QStringLiteral("artist");
    case 3: return QStringLiteral("copyright");
    case 4: return QStringLiteral("character");
    case 5: return QStringLiteral("meta");
    default: return QStringLiteral("general"); // 0, 6 (deprecated), -1, garbage
    }
}

int typeForBucket(const QString &bucket) {
    if (bucket == QStringLiteral("artist"))
        return 1;
    if (bucket == QStringLiteral("copyright"))
        return 3;
    if (bucket == QStringLiteral("character"))
        return 4;
    if (bucket == QStringLiteral("meta"))
        return 5;
    return 0;
}

int parseSafebooruXml(const QByteArray &data, const QString &wanted) {
    // <?xml ...?><tags type="array"><tag type="4" count=".." name=".." .../>
    QXmlStreamReader xml(data);
    bool ok = false;
    int found = kUnknownTag;
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement() && xml.name() == QLatin1String("tag")) {
            const auto attrs = xml.attributes();
            if (attrs.value(QLatin1String("name")).toString() == wanted) {
                found = attrs.value(QLatin1String("type")).toString().toInt(&ok);
                if (!ok)
                    return kTransientFailure;
                return found;
            }
        }
    }
    if (xml.hasError())
        return kTransientFailure;
    return found; // kUnknownTag: well-formed but no exact match
}

int parseYandereJson(const QByteArray &data, const QString &wanted) {
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(data, &err);
    if (err.error != QJsonParseError::NoError || !doc.isArray())
        return kTransientFailure;
    for (const QJsonValue &v : doc.array()) {
        if (!v.isObject())
            continue;
        const QJsonObject o = v.toObject();
        if (o.value(QStringLiteral("name")).toString() == wanted)
            return o.value(QStringLiteral("type")).toInt(kUnknownTag);
    }
    return kUnknownTag;
}

int parseDanbooruTagsJson(const QByteArray &data, const QString &wanted) {
    // [{"name":"hatsune_miku","category":4,...}] - anonymous OK.
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(data, &err);
    if (err.error != QJsonParseError::NoError || !doc.isArray())
        return kTransientFailure;
    for (const QJsonValue &v : doc.array()) {
        if (!v.isObject())
            continue;
        const QJsonObject o = v.toObject();
        if (o.value(QStringLiteral("name")).toString() == wanted)
            return o.value(QStringLiteral("category")).toInt(kUnknownTag);
    }
    return kUnknownTag;
}

} // namespace TagTypes

// ── TagTypeCache ─────────────────────────────────────────────────

bool TagTypeCache::open(const QString &dbPath) {
    close();
    QDir().mkpath(QFileInfo(dbPath).absolutePath()); // index cache may be off
    m_conn = QStringLiteral("tagdb_%1_%2").arg(
        QString::fromLatin1(QCryptographicHash::hash(dbPath.toUtf8(),
                                                     QCryptographicHash::Sha1)
                                 .toHex()
                                 .left(8)),
        QUuid::createUuid().toString(QUuid::WithoutBraces));
    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_conn);
    db.setDatabaseName(dbPath);
    if (!db.open()) {
        QSqlDatabase::removeDatabase(m_conn);
        m_conn.clear();
        return false;
    }
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS tag_types ("
                               "tag TEXT PRIMARY KEY, type INTEGER)"))) {
        close();
        return false;
    }
    m_open = true;
    return true;
}

bool TagTypeCache::beginBatch() {
    if (!m_open)
        return false;
    return QSqlDatabase::database(m_conn).transaction();
}

bool TagTypeCache::endBatch(bool commit) {
    if (!m_open)
        return false;
    QSqlDatabase db = QSqlDatabase::database(m_conn);
    return commit ? db.commit() : db.rollback();
}

bool TagTypeCache::get(const QString &tag, int &type) {
    if (!m_open)
        return false;
    QSqlQuery q(QSqlDatabase::database(m_conn));
    q.prepare(QStringLiteral("SELECT type FROM tag_types WHERE tag=?"));
    q.addBindValue(tag);
    if (!q.exec() || !q.next())
        return false;
    type = q.value(0).toInt();
    return true;
}

void TagTypeCache::put(const QString &tag, int type) {
    if (!m_open)
        return;
    QSqlQuery q(QSqlDatabase::database(m_conn));
    q.prepare(QStringLiteral("INSERT OR REPLACE INTO tag_types (tag, type) VALUES (?, ?)"));
    q.addBindValue(tag);
    q.addBindValue(type);
    q.exec();
}

void TagTypeCache::putBatch(const QHash<QString, int> &types) {
    for (auto it = types.begin(); it != types.end(); ++it)
        put(it.key(), it.value());
}

void TagTypeCache::close() {
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

// ── TagTypeFetcher ───────────────────────────────────────────────

namespace {
QUrl safebooruUrl(const QString &tag) {
    return QUrl(QStringLiteral("https://safebooru.org/index.php?page=dapi&s=tag"
                               "&q=index&name=") +
                QString::fromLatin1(QUrl::toPercentEncoding(tag)));
}

QUrl gelbooruUrl(const QString &tag, const QString &userId, const QString &apiKey) {
    // Gelbooru's tag endpoint answers anonymously; credentials (when set)
    // just raise the rate limit. Never send half a credential pair.
    QUrl url(QStringLiteral("https://gelbooru.com/index.php?page=dapi&s=tag"
                            "&q=index&name=") +
             QString::fromLatin1(QUrl::toPercentEncoding(tag)));
    if (!userId.isEmpty() && !apiKey.isEmpty()) {
        url.setQuery(url.query() +
                     QStringLiteral("&api_key=") +
                     QString::fromLatin1(QUrl::toPercentEncoding(apiKey)) +
                     QStringLiteral("&user_id=") +
                     QString::fromLatin1(QUrl::toPercentEncoding(userId)));
    }
    return url;
}

QUrl yandereUrl(const QString &tag) {
    return QUrl(QStringLiteral("https://yande.re/tag.json?name=") +
                QString::fromLatin1(QUrl::toPercentEncoding(tag)));
}

QUrl danbooruUrl(const QString &tag) {
    return QUrl(QStringLiteral("https://danbooru.donmai.us/tags.json?search[name]=") +
                QString::fromLatin1(QUrl::toPercentEncoding(tag)) +
                QStringLiteral("&limit=5"));
}

QNetworkRequest timedRequest(const QUrl &url) {
    QNetworkRequest req(url);
    req.setTransferTimeout(15000);
    RemoteSource::applyCookies(req); // clearance header, if any
    req.setHeader(QNetworkRequest::UserAgentHeader,
                  QStringLiteral("booru-explorer/0.1 (local tag classifier)"));
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    return req;
}
} // namespace

TagTypeFetcher::TagTypeFetcher(QObject *parent)
    : QObject(parent), m_nam(new QNetworkAccessManager(this)),
      m_backoff(new QTimer(this)) {
    m_backoff->setSingleShot(true);
    m_backoff->setInterval(kBackoffMs);
    connect(m_backoff, &QTimer::timeout, this, [this] {
        if (m_active)
            pump();
    });
}

void TagTypeFetcher::setGelbooruCredentials(const QString &userId,
                                             const QString &apiKey) {
    m_gelbooruUser = userId.trimmed();
    m_gelbooruKey = apiKey.trimmed();
}

void TagTypeFetcher::fetch(const QStringList &tags, const QString &dbPath) {
    cancel();
    m_cache.open(dbPath); // best effort; results still emitted on failure
    m_order.clear();
    // Gelbooru needs credentials (its tag API 401s anonymously); without
    // them the anonymous Danbooru index goes first — same tag universe,
    // same category numbering. Safebooru (SFW subset) then yandere cover
    // the misses.
    if (!m_gelbooruUser.isEmpty() && !m_gelbooruKey.isEmpty())
        m_order << Source::Gelbooru;
    m_order << Source::Danbooru << Source::Safebooru << Source::Yandere;
    m_gelbooruDead = false;
    QSet<QString> seen;
    for (const QString &t : tags) {
        const QString tag = t.trimmed().toLower();
        if (!tag.isEmpty() && !seen.contains(tag)) {
            seen.insert(tag);
            m_queue.append(tag);
        }
    }
    m_done = 0;
    m_total = m_queue.size();
    m_failures = 0;
    m_results.clear();
    m_cache.beginBatch();
    if (m_total == 0) {
        m_cache.endBatch(true);
        m_cache.close();
        emit finished(m_results);
        return;
    }
    m_active = true;
    emit progress(0, m_total);
    pump();
}

void TagTypeFetcher::cancel() {
    m_backoff->stop();
    for (Flight *f : m_flights) {
        if (f->reply) {
            // Detach first: an aborted reply still emits finished, and the
            // flight is gone by then.
            QObject::disconnect(f->reply, nullptr, this, nullptr);
            f->reply->abort();
            f->reply->deleteLater();
        }
        delete f;
    }
    m_flights.clear();
    m_queue.clear();
    m_cache.endBatch(true); // keep verdicts fetched so far
    m_cache.close();
    m_active = false;
}

void TagTypeFetcher::pump() {
    if (!m_active)
        return;
    if (!m_backoff->isActive()) {
        while (m_flights.size() < kMaxFlight && !m_queue.isEmpty()) {
            auto *flight = new Flight{m_queue.takeFirst(), nullptr, 0, 0};
            // Skip a Gelbooru stage already proven dead this run.
            if (m_gelbooruDead && m_order.value(flight->stage) == Source::Gelbooru)
                flight->stage++;
            issue(flight);
            if (flight->reply)
                m_flights.append(flight);
            else
                delete flight; // no sources left (all skipped)
        }
    }
    if (m_flights.isEmpty()) {
        if (!m_queue.isEmpty())
            return; // backed off: the timer will resume us
        m_active = false;
        m_cache.endBatch(true);
        m_cache.close();
        emit finished(m_results);
    }
}

void TagTypeFetcher::issue(Flight *flight) {
    while (flight->stage < m_order.size()) {
        const Source src = m_order.at(flight->stage);
        if (src == Source::Gelbooru &&
            (m_gelbooruDead || m_gelbooruUser.isEmpty() || m_gelbooruKey.isEmpty())) {
            flight->stage++;
            continue;
        }
        QUrl url;
        if (src == Source::Gelbooru)
            url = gelbooruUrl(flight->tag, m_gelbooruUser, m_gelbooruKey);
        else if (src == Source::Danbooru)
            url = danbooruUrl(flight->tag);
        else if (src == Source::Safebooru)
            url = safebooruUrl(flight->tag);
        else
            url = yandereUrl(flight->tag);
        flight->reply = m_nam->get(timedRequest(url));
        connect(flight->reply, &QNetworkReply::finished, this,
                [this, flight] { onReply(flight); });
        return;
    }
    flight->reply = nullptr;
}

int TagTypeFetcher::parseFor(Source src, const QByteArray &body, const QString &tag) {
    if (src == Source::Yandere)
        return TagTypes::parseYandereJson(body, tag);
    if (src == Source::Danbooru)
        return TagTypes::parseDanbooruTagsJson(body, tag);
    return TagTypes::parseSafebooruXml(body, tag); // Gelbooru shares the schema
}

void TagTypeFetcher::onReply(Flight *flight) {
    m_flights.removeAll(flight);
    QNetworkReply *reply = flight->reply;
    const QNetworkReply::NetworkError err = reply->error();
    const QByteArray body = (err == QNetworkReply::NoError) ? reply->readAll() : QByteArray();
    const int http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    reply->deleteLater();

    const Source src = m_order.value(flight->stage, Source::Safebooru);
    if (http == 429 && flight->attempts + 1 < kMaxAttempts) {
        // Rate-limited: requeue the same tag and pause launches briefly.
        // (The fresh flight restarts at stage 0; verdicts are idempotent,
        // so worst case is a few extra requests.)
        flight->attempts++;
        m_queue.prepend(flight->tag);
        reply->deleteLater();
        delete flight;
        if (!m_backoff->isActive()) {
            m_backoff->start();
            emit backoff();
        }
        pump();
        return;
    }
    if (src == Source::Gelbooru && (http == 401 || http == 403)) {
        // Gelbooru refused us (bad credentials, or anonymous blocked):
        // stop hitting it this run, fall through to the public sources,
        // and tell the UI once.
        if (!m_gelbooruDead) {
            m_gelbooruDead = true;
            emit gelbooruAuthFailed();
        }
        flight->stage++;
        issue(flight);
        if (flight->reply) {
            m_flights.append(flight);
            pump();
        } else {
            const QString tag = flight->tag;
            delete flight;
            Q_UNUSED(tag);
            m_done++;
            emit progress(m_done, m_total);
            pump();
        }
        return;
    }

    int type = TagTypes::kTransientFailure;
    if (err == QNetworkReply::NoError)
        type = parseFor(src, body, flight->tag);

    if (type == TagTypes::kUnknownTag && flight->stage + 1 < m_order.size()) {
        // Miss at this source: try the next one.
        flight->stage++;
        flight->reply = nullptr;
        issue(flight);
        if (flight->reply) {
            m_flights.append(flight);
            pump();
            return;
        }
        // else: no sources left below; fall through as unresolvable
    }

    const QString tag = flight->tag;
    delete flight;
    if (type >= TagTypes::kUnknownTag) {
        // Definitive verdict (found bucket or confirmed miss): cache it.
        m_cache.put(tag, type);
        if (type != TagTypes::kUnknownTag)
            m_results.insert(tag, TagTypes::bucketForType(type));
    } else {
        // Transient failure - leave uncached so the next scan retries.
        m_failures++;
    }
    m_done++;
    emit progress(m_done, m_total);
    pump();
}

void TagTypeFetcher::validateCredentials(const QString &userId, const QString &apiKey) {
    QNetworkReply *reply =
        m_nam->get(timedRequest(gelbooruUrl(QStringLiteral("hatsune_miku"),
                                            userId.trimmed(), apiKey.trimmed())));
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        const int http =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        bool ok = false;
        QString msg;
        if (reply->error() != QNetworkReply::NoError) {
            msg = http > 0 ? QStringLiteral("HTTP %1 (%2)")
                                   .arg(http)
                                   .arg(reply->errorString())
                           : QStringLiteral("Network error: %1").arg(reply->errorString());
        } else {
            const int type = TagTypes::parseSafebooruXml(reply->readAll(),
                                                         QStringLiteral("hatsune_miku"));
            if (type == TagTypes::kTransientFailure) {
                msg = QStringLiteral("HTTP 200 but the response did not parse");
            } else {
                ok = true;
                msg = QStringLiteral("OK - hatsune_miku resolves as %1")
                          .arg(TagTypes::bucketForType(type));
            }
        }
        reply->deleteLater();
        emit validated(ok, msg);
    });
}
