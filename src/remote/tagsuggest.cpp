#include "tagsuggest.h"

#include "index.h"
#include "remotesource.h"
#include "tagtypes.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QUrl>
#include <QUrlQuery>
#include <QtConcurrent>

namespace {
QNetworkRequest timedRequest(const QUrl &url) {
    QNetworkRequest req(url);
    req.setTransferTimeout(15000);
    RemoteSource::applyCookies(req); // clearance header, if any
    req.setHeader(QNetworkRequest::UserAgentHeader,
                  QStringLiteral("booru-explorer/0.1 (local booru browser)"));
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    return req;
}

QString szuruBucket(const QString &category) {
    const QString c = category.toLower();
    if (c == QStringLiteral("artist"))
        return QStringLiteral("artist");
    if (c == QStringLiteral("character"))
        return QStringLiteral("character");
    if (c == QStringLiteral("copyright") || c == QStringLiteral("series") ||
        c == QStringLiteral("studio"))
        return QStringLiteral("copyright");
    if (c == QStringLiteral("meta"))
        return QStringLiteral("meta");
    return QStringLiteral("general");
}

QVector<TagSuggestion> takeCapped(QVector<TagSuggestion> v, int cap = 12) {
    if (v.size() > cap)
        v.resize(cap);
    return v;
}

QVector<TagSuggestion> parseDanbooru(const QByteArray &body) {
    // [{name, category, post_count}] - category is danbooru numbering.
    QVector<TagSuggestion> out;
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &err);
    if (err.error != QJsonParseError::NoError || !doc.isArray())
        return out;
    for (const QJsonValue &v : doc.array()) {
        if (!v.isObject())
            continue;
        const QJsonObject o = v.toObject();
        const QString name = o.value(QStringLiteral("name")).toString();
        if (name.isEmpty())
            continue;
        TagSuggestion s;
        s.tag = name;
        s.bucket = TagTypes::bucketForType(o.value(QStringLiteral("category")).toInt(0));
        s.count = o.value(QStringLiteral("post_count")).toInt(0);
        out.append(s);
    }
    return takeCapped(out);
}

QVector<TagSuggestion> parseGelbooru(const QByteArray &body) {
    // {"tag": [{name, type, count}]} - Gelbooru tag types match danbooru
    // numbering; bare arrays tolerated.
    QVector<TagSuggestion> out;
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &err);
    if (err.error != QJsonParseError::NoError)
        return out;
    QJsonArray arr;
    if (doc.isObject())
        arr = doc.object().value(QStringLiteral("tag")).toArray();
    else if (doc.isArray())
        arr = doc.array();
    for (const QJsonValue &v : arr) {
        if (!v.isObject())
            continue;
        const QJsonObject o = v.toObject();
        const QString name = o.value(QStringLiteral("name")).toString();
        if (name.isEmpty())
            continue;
        TagSuggestion s;
        s.tag = name;
        s.bucket = TagTypes::bucketForType(o.value(QStringLiteral("type")).toInt(0));
        s.count = o.value(QStringLiteral("count")).toInt(0);
        out.append(s);
    }
    return takeCapped(out);
}

QVector<TagSuggestion> parseMoebooru(const QByteArray &body) {
    // [{name, type, count}]
    QVector<TagSuggestion> out;
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &err);
    if (err.error != QJsonParseError::NoError || !doc.isArray())
        return out;
    for (const QJsonValue &v : doc.array()) {
        if (!v.isObject())
            continue;
        const QJsonObject o = v.toObject();
        const QString name = o.value(QStringLiteral("name")).toString();
        if (name.isEmpty())
            continue;
        TagSuggestion s;
        s.tag = name;
        s.bucket = TagTypes::bucketForType(o.value(QStringLiteral("type")).toInt(0));
        s.count = o.value(QStringLiteral("count")).toInt(0);
        out.append(s);
    }
    return takeCapped(out);
}

QVector<TagSuggestion> parseSzuru(const QByteArray &body) {
    // {"results": [{names: [...], category, usages}]}
    QVector<TagSuggestion> out;
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
        return out;
    for (const QJsonValue &v :
         doc.object().value(QStringLiteral("results")).toArray()) {
        if (!v.isObject())
            continue;
        const QJsonObject o = v.toObject();
        QString name;
        for (const QJsonValue &n : o.value(QStringLiteral("names")).toArray()) {
            if (n.isString() && !n.toString().isEmpty()) {
                name = n.toString();
                break;
            }
        }
        if (name.isEmpty())
            continue;
        TagSuggestion s;
        s.tag = name;
        s.bucket = szuruBucket(o.value(QStringLiteral("category")).toString());
        s.count = o.value(QStringLiteral("usages")).toInt(0);
        out.append(s);
    }
    return takeCapped(out);
}
} // namespace

TagSuggester::TagSuggester(QObject *parent)
    : QObject(parent), m_nam(new QNetworkAccessManager(this)) {}

void TagSuggester::setLocalIndex(const Index *index) {
    m_localIndex = index;
}

void TagSuggester::cancel() {
    if (m_reply) {
        disconnect(m_reply, nullptr, this, nullptr);
        m_reply->abort();
        m_reply->deleteLater();
        m_reply = nullptr;
    }
}

QUrl TagSuggester::urlFor(BooruEngine engine, const QString &baseUrl,
                          const QString &prefix, const SiteCreds &creds) {
    if (baseUrl.isEmpty() || prefix.length() < 2)
        return {};
    if (engine == BooruEngine::Danbooru) {
        QUrl url(baseUrl + QStringLiteral("/tags.json"));
        QUrlQuery q;
        q.addQueryItem(QStringLiteral("search[name_matches]"), prefix + u'*');
        q.addQueryItem(QStringLiteral("search[order]"), QStringLiteral("count"));
        q.addQueryItem(QStringLiteral("limit"), QStringLiteral("12"));
        url.setQuery(q);
        return url;
    }
    if (engine == BooruEngine::Gelbooru) {
        QUrl url(RemotePosts::apiBaseUrl(engine, baseUrl) +
                 QStringLiteral("/index.php"));
        QUrlQuery q;
        q.addQueryItem(QStringLiteral("page"), QStringLiteral("dapi"));
        q.addQueryItem(QStringLiteral("s"), QStringLiteral("tag"));
        q.addQueryItem(QStringLiteral("q"), QStringLiteral("index"));
        q.addQueryItem(QStringLiteral("name_pattern"), prefix + u'%');
        q.addQueryItem(QStringLiteral("limit"), QStringLiteral("12"));
        q.addQueryItem(QStringLiteral("json"), QStringLiteral("1"));
        if (creds.complete()) {
            q.addQueryItem(QStringLiteral("api_key"), creds.secret);
            q.addQueryItem(QStringLiteral("user_id"), creds.user.trimmed());
        }
        url.setQuery(q);
        return url;
    }
    if (engine == BooruEngine::Moebooru) {
        QUrl url(baseUrl + QStringLiteral("/tag.json"));
        QUrlQuery q;
        q.addQueryItem(QStringLiteral("name"), prefix + u'*');
        q.addQueryItem(QStringLiteral("order"), QStringLiteral("count"));
        q.addQueryItem(QStringLiteral("limit"), QStringLiteral("12"));
        url.setQuery(q);
        return url;
    }
    if (engine == BooruEngine::Szurubooru) {
        QUrl url(baseUrl + QStringLiteral("/api/tags/"));
        QUrlQuery q;
        q.addQueryItem(QStringLiteral("query"), prefix + u'*');
        q.addQueryItem(QStringLiteral("limit"), QStringLiteral("12"));
        url.setQuery(q);
        return url;
    }
    return {}; // Shimmie: no tag autocomplete endpoint
}

void TagSuggester::suggest(const BooruSite &site, const SiteCreds &creds,
                           const QString &prefix) {
    cancel();
    const QString cacheKey = site.key + u'\n' + prefix.toLower();
    if (m_cache.contains(cacheKey)) {
        emit finished(site.key, prefix, m_cache.value(cacheKey));
        return;
    }
    if (m_localIndex) {
        // Instant offline answers from the local archive; the remote
        // fetch below replaces them when (and if) it arrives.
        QVector<TagSuggestion> local;
        for (const auto &s : m_localIndex->suggest(prefix, 12)) {
            TagSuggestion t;
            t.tag = s.tag;
            t.bucket = s.bucket;
            t.count = s.count;
            local.append(t);
        }
        if (!local.isEmpty())
            emit finished(site.key, prefix, local);
    }
    const QUrl url = urlFor(site.engine, site.baseUrl, prefix, creds);
    if (!url.isValid() || url.isEmpty()) {
        emit finished(site.key, prefix, {});
        return;
    }    m_siteKey = site.key;
    m_prefix = prefix;
    // Unconditional: clears a stale proxy when the entry drops it.
    m_nam->setProxy(RemotePosts::proxyFromSpec(site.proxy));
    QNetworkRequest req = timedRequest(url);
    if (site.engine == BooruEngine::Danbooru && creds.complete()) {
        req.setRawHeader("Authorization",
                         "Basic " + (creds.user.trimmed() + u':' + creds.secret)
                                        .toUtf8()
                                        .toBase64());
    } else if (site.engine == BooruEngine::Szurubooru) {
        req.setRawHeader("Accept", "application/json");
        if (creds.complete())
            req.setRawHeader(
                "Authorization",
                "Token " + (creds.user.trimmed() + u':' + creds.secret).toUtf8().toBase64());
    }
    // Fingerprint-walled hosts: suggestions ride the browser stack like
    // searches do (plain requests get wall pages there). Off the UI
    // thread: keystroke latency must not wait on a profile round trip.
    // Web results bypass m_cache (it lives on the caller thread).
    const RemotePosts::SiteQuirk *wq =
        RemotePosts::quirkForHost(QUrl(site.baseUrl).host().toLower());
    if (wq && wq->powWall) {
        if (const RemoteSource::WebFetchFn web = RemoteSource::webFetcher()) {
            const BooruEngine engine = site.engine;
            QPointer<TagSuggester> self(this);
            const QString key = m_siteKey, prefix = m_prefix;
            (void)QtConcurrent::run([web, url, engine, key, prefix, self] {
                const RemoteSource::WebFetchResult fetched = web(url, 20000);
                const QVector<TagSuggestion> out =
                    fetched.error.isEmpty() && !fetched.body.isEmpty()
                        ? parse(engine, fetched.body)
                        : QVector<TagSuggestion>{};
                if (self)
                    emit self->finished(key, prefix, out);
            });
            return;
        }
    }
    m_reply = m_nam->get(req);
    connect(m_reply, &QNetworkReply::finished, this, [this] {
        QNetworkReply *reply = m_reply;
        m_reply = nullptr;
        const QString key = m_siteKey, prefix = m_prefix;
        QVector<TagSuggestion> out;
        if (reply && reply->error() == QNetworkReply::NoError) {
            const BooruSite *site = RemoteSource::siteFor(key);
            out = parse(site ? site->engine : BooruEngine::Danbooru,
                        reply->readAll());
            if (m_cache.size() > 300)
                m_cache.clear();
            m_cache.insert(key + u'\n' + prefix.toLower(), out);
        }
        if (reply)
            reply->deleteLater();
        emit finished(key, prefix, out);
    });
}

QVector<TagSuggestion> TagSuggester::parse(BooruEngine engine,
                                           const QByteArray &body) {
    switch (engine) {
    case BooruEngine::Danbooru:
        return parseDanbooru(body);
    case BooruEngine::Gelbooru:
    case BooruEngine::Safebooru:
        return parseGelbooru(body);
    case BooruEngine::Moebooru:
        return parseMoebooru(body);
    case BooruEngine::Szurubooru:
        return parseSzuru(body);
    case BooruEngine::Shimmie:
        return {};
    }
    return {};
}
