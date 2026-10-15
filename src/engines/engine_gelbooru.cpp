// Gelbooru engine (dapi family: Gelbooru + Safebooru): index.php listing,
// {"post": [...]} or bare-array shapes, credentials in the query string.
// The rule34.xxx captcha wall lives on the main host, so API calls go to
// the api subdomain (sitequirks table, via apiBase()).
#include "booruengine.h"
#include "explicittags.h"

#include <QUrlQuery>

namespace {
class EngineGelbooru : public BooruEngineOps {
public:
    QUrl searchUrl(const Context &ctx, const QStringList &tags, int page,
                   int limit) const override {
        QUrl url(apiBase(ctx) + QStringLiteral("/index.php"));
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("page"), QStringLiteral("dapi"));
        query.addQueryItem(QStringLiteral("s"), QStringLiteral("post"));
        query.addQueryItem(QStringLiteral("q"), QStringLiteral("index"));
        query.addQueryItem(QStringLiteral("tags"), joinTags(tags));
        query.addQueryItem(QStringLiteral("pid"), QString::number(qMax(0, page - 1)));
        query.addQueryItem(QStringLiteral("limit"), QString::number(qBound(1, limit, 100)));
        query.addQueryItem(QStringLiteral("json"), QStringLiteral("1"));
        if (ctx.engine == BooruEngine::Gelbooru && ctx.creds.complete()) {
            query.addQueryItem(QStringLiteral("api_key"), ctx.creds.secret);
            query.addQueryItem(QStringLiteral("user_id"), ctx.creds.user.trimmed());
        }
        url.setQuery(query);
        return url;
    }
    SearchResult parseSearch(const QString &siteKey, const Context &ctx,
                             const QByteArray &body) const override {
        SearchResult result;
        result.site = siteKey;
        const QJsonDocument doc = parseJsonBody(body);
        if (doc.isNull()) {
            result.error = QStringLiteral("Unparseable response");
            return result;
        }
        // Newer dapi JSON: {"post": [ ... ]}; older answers a bare array.
        QJsonArray arr;
        if (doc.isObject())
            arr = doc.object().value(QStringLiteral("post")).toArray();
        else if (doc.isArray())
            arr = doc.array();
        else {
            result.error = QStringLiteral("Unexpected response shape");
            return result;
        }
        for (const QJsonValue &v : arr) {
            if (v.isObject())
                result.posts.append(
                    RemotePosts::fromGelbooru(v.toObject(), ctx.baseUrl));
        }
        return result;
    }
    QUrl refreshUrl(const Context &ctx, qint64 id) const override {
        QUrl u(apiBase(ctx) + QStringLiteral("/index.php"));
        QUrlQuery q;
        q.addQueryItem(QStringLiteral("page"), QStringLiteral("dapi"));
        q.addQueryItem(QStringLiteral("s"), QStringLiteral("post"));
        q.addQueryItem(QStringLiteral("q"), QStringLiteral("index"));
        q.addQueryItem(QStringLiteral("id"), QString::number(id));
        q.addQueryItem(QStringLiteral("json"), QStringLiteral("1"));
        if (ctx.creds.complete()) {
            q.addQueryItem(QStringLiteral("api_key"), ctx.creds.secret);
            q.addQueryItem(QStringLiteral("user_id"), ctx.creds.user.trimmed());
        }
        u.setQuery(q);
        return u;
    }
    QJsonObject parseSingle(const QByteArray &body, bool &ok) const override {
        ok = false;
        const QJsonDocument doc = parseJsonBody(body);
        if (doc.isNull())
            return {};
        if (doc.isObject())
            return firstOfArray(doc.object().value(QStringLiteral("post")).toArray(), ok);
        return firstOfArray(doc.array(), ok);
    }
    void applyAuth(QNetworkRequest &, const SiteCreds &) const override {
        // Credentials already ride in the query string; nothing to sign.
    }
};
} // namespace

namespace EngineInternal {
const BooruEngineOps *gelbooruOps() {
    static const EngineGelbooru ops;
    return &ops;
}
} // namespace EngineInternal

namespace RemotePosts {
Post fromGelbooru(const QJsonObject &o, const QString &baseUrl) {
    Post p;
    p.id = BooruEngineOps::jsonInt(o.value(QStringLiteral("id")));
    p.score = static_cast<int>(BooruEngineOps::jsonInt(o.value(QStringLiteral("score"))));
    p.rating = normalizeRating(o.value(QStringLiteral("rating")).toString());
    p.tagsGeneral = o.value(QStringLiteral("tags")).toString();
    p.untyped = p.tagsGeneral; // one flat blob: classifiable later
    p.width = static_cast<int>(BooruEngineOps::jsonInt(o.value(QStringLiteral("width"))));
    p.height = static_cast<int>(BooruEngineOps::jsonInt(o.value(QStringLiteral("height"))));
    p.source = o.value(QStringLiteral("source")).toString();
    p.date = o.value(QStringLiteral("created_at")).toString();
    p.fileUrl = BooruEngineOps::resolveFileUrl(baseUrl, o.value(QStringLiteral("file_url")).toString());
    p.sampleUrl = BooruEngineOps::resolveFileUrl(baseUrl, o.value(QStringLiteral("sample_url")).toString());
    p.previewUrl = BooruEngineOps::resolveFileUrl(baseUrl, o.value(QStringLiteral("preview_url")).toString());
    p.ext = BooruEngineOps::extOf(p.fileUrl);
    const QString site = baseUrl.contains(QStringLiteral("safebooru"))
                             ? QStringLiteral("Safebooru")
                             : QStringLiteral("Gelbooru");
    p.postUrl = postPageUrl(site == QStringLiteral("Safebooru") ? QStringLiteral("safebooru")
                                                                : QStringLiteral("gelbooru"),
                            baseUrl, p.id);
    BooruEngineOps::finishRemote(p, site, o, BooruEngine::Gelbooru);
    return p;
}
} // namespace RemotePosts
