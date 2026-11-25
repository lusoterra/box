#include "hydrus.h"

#include "post.h" // cleanTagTokens
#include "tagcategories.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSet>
#include <QUrl>
#include <QUrlQuery>

namespace {

QString normalizedBase(const QString &baseUrl) {
    QString b = baseUrl.trimmed();
    if (b.isEmpty())
        b = QStringLiteral("http://127.0.0.1:45869");
    if (!b.contains(QStringLiteral("://")))
        b.prepend(QStringLiteral("http://"));
    while (b.endsWith(u'/'))
        b.chop(1);
    return b;
}

QString replyBody(const QByteArray &body) {
    QString text = QString::fromUtf8(body).trimmed();
    if (text.size() > 300)
        text = text.left(300) + QStringLiteral("…");
    return text;
}

// Tags of one tag service, storage tags first (display tags add parent
// names that are not really assigned to the file).
void appendServiceTags(const QJsonObject &service, QStringList &out,
                       QSet<QString> &seen) {
    const auto appendMap = [&out, &seen](const QJsonObject &map) {
        if (map.isEmpty())
            return;
        // "0" = the actual tag. Older servers may omit it; concatenate
        // the rest then (duplicates are removed by the caller).
        const bool hasPlain = map.contains(QStringLiteral("0"));
        for (auto it = map.begin(); it != map.end(); ++it) {
            if (hasPlain && it.key() != QStringLiteral("0"))
                continue;
            if (!it.value().isArray())
                continue;
            for (const QJsonValue &v : it.value().toArray()) {
                if (!v.isString())
                    continue;
                const QString tag = v.toString().trimmed();
                if (tag.isEmpty())
                    continue;
                const QString key = tag.toLower();
                if (seen.contains(key))
                    continue;
                seen.insert(key);
                out << tag;
            }
        }
    };
    const QJsonObject storage = service.value(QStringLiteral("storage_tags"))
                                    .toObject();
    if (!storage.isEmpty())
        appendMap(storage);
    else
        appendMap(service.value(QStringLiteral("display_tags")).toObject());
}

QString namespaceBucket(const QString &tag, QString *stripped) {
    if (stripped)
        stripped->clear();
    const qsizetype colon = tag.indexOf(u':');
    if (colon <= 0)
        return {};
    const QString ns = tag.left(colon).trimmed().toLower();
    const QString rest = tag.mid(colon + 1).trimmed();
    if (rest.isEmpty())
        return {};
    const QString bucket = TagCategories::hydrusBucket(ns);
    if (bucket.isEmpty())
        return {};
    if (stripped)
        *stripped = rest;
    return bucket;
}

void addUnique(QStringList &list, const QString &tag) {
    if (!tag.isEmpty() && !list.contains(tag))
        list.append(tag);
}

// Hydrus tags are free-form strings (apostrophes, commas, quotes and
// spaces are legal). Boilerplate hygiene from booru imports must not
// silently drop them here; only trim and dedupe case-insensitively.
QStringList cleanHydrusTags(const QStringList &tags) {
    QStringList out;
    QSet<QString> seen;
    for (const QString &raw : tags) {
        const QString tag = raw.trimmed();
        if (tag.isEmpty())
            continue;
        const QString key = tag.toLower();
        if (seen.contains(key))
            continue;
        seen.insert(key);
        out << tag;
    }
    out.sort(Qt::CaseInsensitive);
    return out;
}

} // namespace

namespace Hydrus {

QString hashKindLabel(HashKind kind) {
    switch (kind) {
    case HashKind::Sha1:
        return QStringLiteral("SHA-1");
    case HashKind::Md5:
        return QStringLiteral("MD5");
    case HashKind::Sha256:
    default:
        return QStringLiteral("SHA-256");
    }
}

QString hashKindKey(HashKind kind) {
    switch (kind) {
    case HashKind::Sha1:
        return QStringLiteral("sha1");
    case HashKind::Md5:
        return QStringLiteral("md5");
    case HashKind::Sha256:
    default:
        return QStringLiteral("sha256");
    }
}

QString hashFor(HashKind kind, const FileContentHashes &hashes) {
    switch (kind) {
    case HashKind::Sha1:
        return hashes.sha1;
    case HashKind::Md5:
        return hashes.md5;
    case HashKind::Sha256:
    default:
        return hashes.sha256;
    }
}

QUrl metadataUrl(const QString &baseUrl, const QString &hash) {
    QUrl url(normalizedBase(baseUrl) +
             QStringLiteral("/get_files/file_metadata"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("hash"), hash.trimmed());
    query.addQueryItem(QStringLiteral("include_services_object"),
                       QStringLiteral("false"));
    url.setQuery(query);
    return url;
}

QUrl tagCountsUrl(const QString &baseUrl, const QStringList &tags) {
    QUrl url(normalizedBase(baseUrl) + QStringLiteral("/hyd/tag_counts"));
    QJsonArray arr;
    for (const QString &t : tags) {
        const QString tag = t.trimmed().toLower();
        if (!tag.isEmpty())
            arr.append(tag);
    }
    QUrlQuery query;
    query.addQueryItem(
        QStringLiteral("tags"),
        QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)));
    url.setQuery(query);
    return url;
}

TagCounts parseTagCounts(const QByteArray &body) {
    TagCounts out;
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        out.error = QCoreApplication::translate(
            "Hydrus", "Invalid Hydrus tag-count response");
        return out;
    }
    const QJsonObject root = doc.object();
    const QJsonValue countsValue = root.value(QStringLiteral("counts"));
    if (!countsValue.isObject()) {
        out.error = QCoreApplication::translate(
            "Hydrus", "Hydrus tag-count response has no counts object");
        return out;
    }
    const QJsonObject counts = countsValue.toObject();
    for (auto it = counts.begin(); it != counts.end(); ++it) {
        const QString tag = it.key().trimmed().toLower();
        if (!tag.isEmpty())
            out.counts.insert(tag, it.value().toInt());
    }
    for (const QJsonValue &v :
         root.value(QStringLiteral("unknown")).toArray()) {
        if (!v.isString())
            continue;
        const QString tag = v.toString().trimmed().toLower();
        if (!tag.isEmpty())
            out.unknown << tag;
    }
    out.ok = true;
    return out;
}

FileTags parseMetadata(const QByteArray &body, const QString &wantedHash) {
    FileTags out;
    out.hash = wantedHash;
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        out.error = QCoreApplication::translate("Hydrus",
                                                "Invalid Hydrus response");
        return out;
    }
    const QJsonObject root = doc.object();
    const QJsonValue metadataValue = root.value(QStringLiteral("metadata"));
    if (!metadataValue.isArray()) {
        out.error = QCoreApplication::translate(
            "Hydrus", "Hydrus response has no metadata list");
        return out;
    }

    QJsonObject pick;
    for (const QJsonValue &value : metadataValue.toArray()) {
        if (!value.isObject())
            continue;
        const QJsonObject row = value.toObject();
        const QJsonValue id = row.value(QStringLiteral("file_id"));
        if (id.isNull() || id.isUndefined() ||
            (id.isDouble() && id.toDouble() <= 0))
            continue;
        const QString rowHash =
            row.value(QStringLiteral("hash")).toString();
        if (!wantedHash.isEmpty() && !rowHash.isEmpty() &&
            rowHash.compare(wantedHash, Qt::CaseInsensitive) != 0)
            continue;
        pick = row;
        break;
    }

    out.ok = true;
    if (pick.isEmpty())
        return out; // parsed fine, no such hash
    out.found = true;
    const QJsonValue id = pick.value(QStringLiteral("file_id"));
    out.fileId = id.isDouble() ? static_cast<qint64>(id.toDouble())
                               : id.toString().toLongLong();
    const QString serverHash = pick.value(QStringLiteral("hash")).toString();
    if (!serverHash.isEmpty())
        out.hash = serverHash;
    const QJsonObject services = pick.value(QStringLiteral("tags")).toObject();
    QSet<QString> seen;
    for (auto it = services.begin(); it != services.end(); ++it) {
        if (!it.value().isObject())
            continue;
        appendServiceTags(it.value().toObject(), out.tags, seen);
    }
    out.tags = cleanHydrusTags(out.tags);
    out.tags.sort(Qt::CaseInsensitive);
    return out;
}

CategorizedTags categorize(const QStringList &tags) {
    CategorizedTags out;
    for (const QString &tag : cleanTagTokens(tags)) {
        QString stripped;
        const QString bucket = namespaceBucket(tag, &stripped);
        if (bucket == QStringLiteral("artist"))
            addUnique(out.artist, stripped);
        else if (bucket == QStringLiteral("character"))
            addUnique(out.character, stripped);
        else if (bucket == QStringLiteral("copyright"))
            addUnique(out.copyright, stripped);
        else if (bucket == QStringLiteral("meta"))
            addUnique(out.meta, stripped);
        else
            addUnique(out.general, tag);
        if (tag.startsWith(QStringLiteral("rating:"), Qt::CaseInsensitive) &&
            out.rating.isEmpty()) {
            const QString rating = tag.mid(7).trimmed().toLower();
            if (!rating.isEmpty())
                out.rating = rating;
        }
    }
    return out;
}

QString verifySummary(const QByteArray &body) {
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
        return QCoreApplication::translate(
            "Hydrus", "Connected (unreadable version reply)");
    const QJsonObject o = doc.object();
    QString name = o.value(QStringLiteral("name")).toString().trimmed();
    if (name.isEmpty())
        name = QStringLiteral("Hydrus");
    const int version = o.value(QStringLiteral("hydrus_version")).toInt();
    if (version > 0)
        return QStringLiteral("%1 (Hydrus %2)").arg(name).arg(version);
    const QString api = o.value(QStringLiteral("version")).toString();
    if (!api.isEmpty())
        return QStringLiteral("%1 (API %2)").arg(name, api);
    return name;
}

Client::Client(const Config &config, QObject *parent)
    : QObject(parent), m_nam(new QNetworkAccessManager(this)),
      m_config(config) {
    if (m_config.proxy.type() != QNetworkProxy::NoProxy)
        m_nam->setProxy(m_config.proxy);
}

void Client::cancel() {
    if (!m_reply)
        return;
    QNetworkReply *reply = m_reply;
    m_reply = nullptr;
    disconnect(reply, nullptr, this, nullptr);
    reply->abort();
    reply->deleteLater();
}

void Client::verify() {
    m_request = Request::Verify;
    send(QUrl(normalizedBase(m_config.baseUrl) +
              QStringLiteral("/verify_access_key")));
}

void Client::lookup(const QString &hashHex, HashKind kind) {
    m_kind = kind;
    m_hash = hashHex.trimmed();
    m_request = Request::Metadata;
    send(metadataUrl(m_config.baseUrl, m_hash));
}

void Client::tagCounts(const QStringList &tags) {
    m_request = Request::TagCounts;
    send(tagCountsUrl(m_config.baseUrl, tags));
}

void Client::send(const QUrl &url) {
    cancel();
    QNetworkRequest req(url);
    req.setTransferTimeout(15000);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setHeader(QNetworkRequest::UserAgentHeader,
                  QStringLiteral("box/0.1 (hydrus tags)"));
    req.setRawHeader("Accept", "application/json");
    if (!m_config.accessKey.trimmed().isEmpty())
        req.setRawHeader("Hydrus-Client-API-Access-Key",
                         m_config.accessKey.trimmed().toUtf8());
    QNetworkReply *reply = m_nam->get(req);
    m_reply = reply;
    const HashKind kind = m_kind;
    connect(reply, &QNetworkReply::finished, this, [this, reply, kind] {
        if (m_reply != reply)
            return;
        m_reply = nullptr;
        reply->deleteLater();
        const int status = reply->attribute(
                                   QNetworkRequest::HttpStatusCodeAttribute)
                               .toInt();
        const QByteArray body = reply->readAll();
        if (reply->error() != QNetworkReply::NoError || status >= 400) {
            QString message = replyBody(body);
            if (message.isEmpty())
                message = reply->errorString();
            if (m_request == Request::Verify) {
                emit verified(false, QStringLiteral("Connection failed: %1")
                                         .arg(message));
            } else if (m_request == Request::TagCounts) {
                emit tagCountsFailed(message, status);
            } else {
                emit error(message);
            }
            return;
        }
        if (m_request == Request::Verify) {
            emit verified(true, verifySummary(body));
            return;
        }
        if (m_request == Request::TagCounts) {
            const TagCounts parsed = parseTagCounts(body);
            if (!parsed.ok) {
                emit tagCountsFailed(parsed.error, status);
                return;
            }
            emit tagCountsReady(parsed.counts, parsed.unknown);
            return;
        }
        const FileTags info = parseMetadata(body, m_hash);
        if (!info.ok) {
            emit error(info.error);
            return;
        }
        if (info.found)
            emit found(info, kind);
        else
            emit notFound(kind);
    });
}

} // namespace Hydrus
