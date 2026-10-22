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
#include <QSet>
#include <QUrl>
#include <QUrlQuery>
#include <QXmlStreamReader>
#include <QtConcurrent>

#include <algorithm>

namespace {
QNetworkRequest timedRequest(const QUrl &url) {
    QNetworkRequest req(url);
    req.setTransferTimeout(15000);
    RemoteSource::applyCookies(req); // clearance header, if any
    req.setHeader(QNetworkRequest::UserAgentHeader,
                  QStringLiteral("box/0.1 (local booru browser)"));
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

QVector<TagSuggestion> parseGelbooruXmlList(const QByteArray &body);
QVector<TagSuggestion> parseHybooru(const QByteArray &body);
QVector<TagSuggestion> parseNuuru(const QByteArray &body);
QVector<TagSuggestion> parseHiddenbooru(const QByteArray &body);

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
    return takeCapped(out, 24);
}

QVector<TagSuggestion> parseGelbooru(const QByteArray &body) {
    // {"tag": [{name, type, count}]} - Gelbooru tag types match danbooru
    // numbering; bare arrays tolerated. Older builds (Safebooru) answer
    // the same endpoint with XML instead of JSON - same attributes.
    if (body.trimmed().startsWith('<'))
        return parseGelbooruXmlList(body);
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
    return takeCapped(out, 24);
}

QVector<TagSuggestion> parseGelbooruXmlList(const QByteArray &body) {
    // <tags><tag name=".." type=".." count=".."/>... (older Gelbooru
    // builds, Safebooru): same attributes as the JSON form.
    QVector<TagSuggestion> out;
    QXmlStreamReader xml(body);
    while (!xml.atEnd()) {
        xml.readNext();
        if (!xml.isStartElement() || xml.name() != QLatin1String("tag"))
            continue;
        const auto attrs = xml.attributes();
        const QString name =
            attrs.value(QLatin1String("name")).toString();
        if (name.isEmpty())
            continue;
        bool numOk = false;
        const int type =
            attrs.value(QLatin1String("type")).toString().toInt(&numOk);
        TagSuggestion s;
        s.tag = name;
        s.bucket = TagTypes::bucketForType(numOk ? type : 0);
        s.count = attrs.value(QLatin1String("count")).toString().toInt();
        out.append(s);
        if (out.size() >= 64)
            break; // bounded (takeCapped trims to 12 below)
    }
    return takeCapped(out, 24);
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
    return takeCapped(out, 24);
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
    return takeCapped(out, 24);
}

QVector<TagSuggestion> parseHybooru(const QByteArray &body) {
    // {"tags": {name: usage-count}, ...} — namespaces mirror the detail
    // objects; buckets follow the same mapping as fromHybooru.
    QVector<TagSuggestion> out;
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
        return out;
    const QJsonValue tags = doc.object().value(QStringLiteral("tags"));
    if (!tags.isObject())
        return out;
    const QJsonObject tagObj = tags.toObject();
    for (auto it = tagObj.begin(); it != tagObj.end(); ++it) {        const QString name = it.key().trimmed();
        if (name.isEmpty())
            continue;
        TagSuggestion s;
        const int colon = name.indexOf(u':');
        const QString ns =
            colon > 0 ? name.left(colon).toLower() : QString();
        if (ns == QStringLiteral("character")) {
            s.tag = name.mid(colon + 1).trimmed();
            s.bucket = QStringLiteral("character");
        } else if (ns == QStringLiteral("creator")) {
            s.tag = name.mid(colon + 1).trimmed();
            s.bucket = QStringLiteral("artist");
        } else if (ns == QStringLiteral("series")) {
            s.tag = name.mid(colon + 1).trimmed();
            s.bucket = QStringLiteral("copyright");
        } else if (ns == QStringLiteral("medium") ||
                   ns == QStringLiteral("meta") ||
                   ns == QStringLiteral("title")) {
            s.tag = name.mid(colon + 1).trimmed();
            s.bucket = QStringLiteral("meta");
        } else {
            s.tag = name;
            s.bucket = QStringLiteral("general");
        }
        if (s.tag.isEmpty())
            continue;
        s.count = it.value().toInt(0);
        out.append(s);
    }
    return takeCapped(out, 24);
}

QVector<TagSuggestion> parseNuuru(const QByteArray &body) {
    // Shape unobserved live (search needs a browser session); tolerant:
    // [{name, category, postCount?}] or {"tags": [...]}, mirroring the
    // native taxonomy of the detail objects.
    QVector<TagSuggestion> out;
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &err);
    if (err.error != QJsonParseError::NoError)
        return out;
    QJsonArray arr;
    if (doc.isArray())
        arr = doc.array();
    else if (doc.isObject())
        arr = doc.object().value(QStringLiteral("tags")).toArray();
    for (const QJsonValue &v : arr) {
        if (!v.isObject())
            continue;
        const QJsonObject o = v.toObject();
        const QString name = o.value(QStringLiteral("name")).toString().trimmed();
        if (name.isEmpty())
            continue;
        TagSuggestion s;
        s.tag = name;
        // Same folds as fromNuuru: variant/subvariant read as character.
        const QString ns = o.value(QStringLiteral("category")).toString().toLower();
        if (ns == QStringLiteral("character") || ns == QStringLiteral("variant") ||
            ns == QStringLiteral("subvariant"))
            s.bucket = QStringLiteral("character");
        else if (ns == QStringLiteral("series"))
            s.bucket = QStringLiteral("copyright");
        else if (ns == QStringLiteral("artist"))
            s.bucket = QStringLiteral("artist");
        else if (ns == QStringLiteral("meta") || ns == QStringLiteral("medium") ||
                 ns == QStringLiteral("title"))
            s.bucket = QStringLiteral("meta");
        else
            s.bucket = QStringLiteral("general");
        s.count = o.value(QStringLiteral("postCount")).toInt(
            o.value(QStringLiteral("usages")).toInt(0));
        out.append(s);
    }
    return takeCapped(out, 24);
}
QVector<TagSuggestion> parseHiddenbooru(const QByteArray &body) {
    // Flat array of tag name strings (db->tags->complete).
    QVector<TagSuggestion> out;
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &err);
    if (err.error != QJsonParseError::NoError || !doc.isArray())
        return out;
    for (const QJsonValue &v : doc.array()) {
        QString name;
        if (v.isString())
            name = v.toString();
        else if (v.isObject())
            name = v.toObject().value(QStringLiteral("name")).toString();
        name = name.trimmed();
        if (name.isEmpty() || !tagTokenOk(name))
            continue;
        TagSuggestion s;
        s.tag = name;
        s.bucket = QStringLiteral("general"); // no categories in the API
        out.append(s);
    }
    return takeCapped(out, 24);
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
        q.addQueryItem(QStringLiteral("limit"), QStringLiteral("24"));
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
        q.addQueryItem(QStringLiteral("limit"), QStringLiteral("24"));
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
        q.addQueryItem(QStringLiteral("limit"), QStringLiteral("24"));
        url.setQuery(q);
        return url;
    }
    if (engine == BooruEngine::Szurubooru) {
        QUrl url(baseUrl + QStringLiteral("/api/tags/"));
        QUrlQuery q;
        q.addQueryItem(QStringLiteral("query"), prefix + u'*');
        q.addQueryItem(QStringLiteral("limit"), QStringLiteral("24"));
        url.setQuery(q);
        return url;
    }
    if (engine == BooruEngine::Hybooru) {
        // {"tags": {name: usage-count}}; instance ignores unknown params,
        // so no limit is sent (client trims to the popup cap).
        QUrl url(baseUrl + QStringLiteral("/api/tags"));
        QUrlQuery q;
        q.addQueryItem(QStringLiteral("query"), prefix);
        url.setQuery(q);
        return url;
    }
    if (engine == BooruEngine::Nuuru) {
        QUrl url(baseUrl + QStringLiteral("/api/booru/tags/search"));
        QUrlQuery q;
        q.addQueryItem(QStringLiteral("query"), prefix);
        q.addQueryItem(QStringLiteral("limit"), QStringLiteral("24"));
        url.setQuery(q);
        return url;
    }
    if (engine == BooruEngine::Hiddenbooru) {
        // Flat array of tag name strings.
        QUrl url(baseUrl + QStringLiteral("/api/tag/complete"));
        QUrlQuery q;
        q.addQueryItem(QStringLiteral("term"), prefix);
        url.setQuery(q);
        return url;
    }
    return {}; // Shimmie: no tag autocomplete endpoint
}

void TagSuggester::suggest(const BooruSite &site, const SiteCreds &creds,
                           const QString &prefix) {
    cancel();
    // Metatag heads never hit the network: complete locally from the
    // engine's verified spellings (instant + offline, LoliSnatcher-style).
    if (const QVector<TagSuggestion> meta = metaFor(prefix, site.engine);
        !meta.isEmpty()) {
        emit finished(site.key, prefix, meta);
        return;
    }
    const QString cacheKey = site.key + u'\n' + prefix.toLower();
    if (m_cache.contains(cacheKey)) {
        emit finished(site.key, prefix, m_cache.value(cacheKey));
        return;
    }
    QVector<TagSuggestion> local;
    if (m_localIndex) {
        // Instant offline answers from the local archive; the remote
        // fetch below replaces them when (and if) it arrives.
        for (const auto &s : m_localIndex->suggest(prefix, 24, m_favTags)) {
            TagSuggestion t;
            t.tag = s.tag;
            t.bucket = s.bucket;
            t.count = s.count;
            local.append(t);
        }
    }
    // Your own recent tokens fill the rest: familiar answers even when
    // the index is silent (or absent) and the network is slow or walled.
    appendHistory(local, prefix, m_historyTags, m_localIndex, 24);
    if (!local.isEmpty())
        emit finished(site.key, prefix, local);
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

QVector<TagSuggestion> TagSuggester::metaFor(const QString &bare,
                                               BooruEngine engine) {
    // Metatag head, tolerating a leading exclusion dash (the suggestion
    // completes the value regardless; the caller preserves the dash).
    QString rest = bare.trimmed();
    while (rest.startsWith(u'-') && rest.size() > 1)
        rest = rest.mid(1);
    const int ci = rest.indexOf(u':');
    if (ci <= 0)
        return {};
    const QString head = rest.left(ci).toLower();
    const QString value = rest.mid(ci + 1).toLower();
    QStringList cands;
    if (head == QStringLiteral("sort") && engine == BooruEngine::Nuuru) {
        for (const QString &v :
             {QStringLiteral("date"), QStringLiteral("date_asc"),
              QStringLiteral("date_desc"), QStringLiteral("score"),
              QStringLiteral("score_asc"), QStringLiteral("score_desc"),
              QStringLiteral("controversial"),
              QStringLiteral("controversial_desc"), QStringLiteral("id"),
              QStringLiteral("id_asc"), QStringLiteral("id_desc"),
              QStringLiteral("filesize"), QStringLiteral("filesize_asc"),
              QStringLiteral("filesize_desc"), QStringLiteral("tagcount"),
              QStringLiteral("tagcount_asc"), QStringLiteral("tagcount_desc"),
              QStringLiteral("random")}) {
            if (v.startsWith(value))
                cands << QStringLiteral("sort:") + v;
        }
    } else if (head == QStringLiteral("rating") &&
               engine != BooruEngine::Hybooru &&
               engine != BooruEngine::Nuuru &&
               engine != BooruEngine::Hiddenbooru) {
        for (const QString &v :
             {QStringLiteral("general"), QStringLiteral("sensitive"),
              QStringLiteral("questionable"), QStringLiteral("explicit"),
              QStringLiteral("safe")}) {
            if (v.startsWith(value))
                cands << QStringLiteral("rating:") + v;
        }
    } else if (head == QStringLiteral("order") &&
               engine == BooruEngine::Danbooru) {
        // Mirror exactly what sortSearchTag() would send (single source
        // of truth for verified spellings): the box values it maps.
        for (const QString &box :
             {QStringLiteral("id-asc"), QStringLiteral("score-desc"),
              QStringLiteral("score-asc"), QStringLiteral("random")}) {
            const QString mapped = RemotePosts::sortSearchTag(box, engine);
            if (!mapped.isEmpty() && mapped.startsWith(QStringLiteral("order:")) &&
                mapped.mid(QStringLiteral("order:").size()).startsWith(value))
                cands << mapped;
        }
    } else if (head == QStringLiteral("filetype") &&
               engine == BooruEngine::Danbooru) {
        for (const QString &v :
             {QStringLiteral("jpg"), QStringLiteral("jpeg"),
              QStringLiteral("png"), QStringLiteral("gif"),
              QStringLiteral("webp"), QStringLiteral("webm"),
              QStringLiteral("mp4")}) {
            if (v.startsWith(value))
                cands << QStringLiteral("filetype:") + v;
        }
    }
    QVector<TagSuggestion> out;
    for (const QString &t : cands) {
        TagSuggestion s;
        s.tag = t;
        out.append(s);
    }
    std::sort(out.begin(), out.end(), [](const TagSuggestion &a,
                                         const TagSuggestion &b) {
        return a.tag < b.tag;
    });
    return out;
}

void TagSuggester::appendHistory(QVector<TagSuggestion> &out,
                                   const QString &bare,
                                   const QStringList &history,
                                   const Index *index, int cap) {
    if (bare.size() < 2)
        return;
    QSet<QString> seen;
    for (const TagSuggestion &s : out)
        seen.insert(s.tag.toLower());
    for (const QString &h : history) {
        if (out.size() >= cap)
            break;
        const QString t = h.trimmed();
        if (t.size() < bare.size() || seen.contains(t.toLower()))
            continue;
        if (!t.startsWith(bare, Qt::CaseInsensitive))
            continue;
        TagSuggestion s;
        s.tag = t;
        s.bucket = index ? index->bucketOf(t) : QStringLiteral("general");
        out.append(s);
        seen.insert(t.toLower());
    }
}

QVector<TagSuggestion> TagSuggester::merge(const QString &prefix,
                                              QVector<TagSuggestion> local,
                                              QVector<TagSuggestion> remote,
                                              const QSet<QString> &favTags,
                                              int cap) {
    QVector<TagSuggestion> uni;
    uni.reserve(local.size() + remote.size());
    QSet<QString> seen;
    const QString q = prefix.toLower();
    auto keep = [&](const TagSuggestion &s) {
        const QString key = s.tag.toLower();
        if (key.isEmpty() || seen.contains(key))
            return;
        // The popup only ever completes the current token: rows that
        // neither equal it nor start with it can never display, so they
        // must not eat cap slots either (a count-1M non-match used to
        // crowd out displayable rows).
        if (key != q && !key.startsWith(q))
            return;
        seen.insert(key);
        uni.append(s);
    };
    for (const TagSuggestion &st : local)
        keep(st);
    for (const TagSuggestion &st : remote)
        keep(st);
    QVector<TagSuggestion> out = rank(prefix, uni, favTags);
    if (out.size() > cap)
        out.resize(qMax(0, cap));
    return out;
}

QVector<TagSuggestion> TagSuggester::rank(const QString &prefix,
                                             QVector<TagSuggestion> v,
                                             const QSet<QString> &favTags) {
    const QString q = prefix.toLower();
    std::stable_sort(v.begin(), v.end(),
                     [&q, &favTags](const TagSuggestion &a, const TagSuggestion &b) {
                         const QString al = a.tag.toLower(), bl = b.tag.toLower();
                         const auto tierOf = [&q, &favTags](const QString &l) {
                             if (l == q)
                                 return 0;
                             if (favTags.contains(l))
                                 return l.startsWith(q) ? 1 : 3;
                             return l.startsWith(q) ? 2 : 3;
                         };
                         const int at = tierOf(al), bt = tierOf(bl);
                         if (at != bt)
                             return at < bt;
                         if (a.count != b.count)
                             return a.count > b.count;
                         if (a.tag.size() != b.tag.size())
                             return a.tag.size() > b.tag.size();
                         return al < bl;
                     });
    return v;
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
    case BooruEngine::Hybooru:
        return parseHybooru(body);
    case BooruEngine::Nuuru:
        return parseNuuru(body);
    case BooruEngine::Hiddenbooru:
        return parseHiddenbooru(body);
    case BooruEngine::Shimmie:
        return {};
    }
    return {};
}
