// Szurubooru engine: /api/posts/ listing with offset paging,
// /api/post/<id> refresh, Token auth (406 without the Accept header).
#include "booruengine.h"
#include "explicittags.h"

#include <QUrlQuery>

namespace {
class EngineSzurubooru : public BooruEngineOps {
public:
    QUrl searchUrl(const Context &ctx, const QStringList &tags, int page,
                   int limit) const override {
        QUrl url(ctx.baseUrl + QStringLiteral("/api/posts/"));
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("query"), joinTags(tags));
        query.addQueryItem(QStringLiteral("offset"),
                           QString::number(qMax(0, (page - 1) * qBound(1, limit, 100))));
        query.addQueryItem(QStringLiteral("limit"), QString::number(qBound(1, limit, 100)));
        url.setQuery(query);
        return url;
    }
    SearchResult parseSearch(const QString &siteKey, const Context &ctx,
                             const QByteArray &body) const override {
        SearchResult result;
        result.site = siteKey;
        const QJsonDocument doc = parseJsonBody(body);
        if (doc.isNull() || !doc.isObject() ||
            !doc.object().value(QStringLiteral("results")).isArray()) {
            result.error = doc.isNull() ? QStringLiteral("Unparseable response")
                                        : QStringLiteral("Unexpected response shape");
            return result;
        }
        for (const QJsonValue &v :
             doc.object().value(QStringLiteral("results")).toArray()) {
            if (v.isObject())
                result.posts.append(
                    RemotePosts::fromSzuru(v.toObject(), ctx.baseUrl));
        }
        return result;
    }
    QUrl refreshUrl(const Context &ctx, qint64 id) const override {
        return QUrl(ctx.baseUrl + QStringLiteral("/api/post/") + QString::number(id));
    }
    QJsonObject parseSingle(const QByteArray &body, bool &ok) const override {
        ok = false;
        const QJsonDocument doc = parseJsonBody(body);
        if (doc.isNull())
            return {};
        if (doc.isObject()) {
            ok = true;
            return doc.object();
        }
        return firstOfArray(doc.array(), ok); // tolerate list shape anyway
    }
    void applyAuth(QNetworkRequest &req, const SiteCreds &creds) const override {
        req.setRawHeader("Accept", "application/json"); // 406 otherwise
        if (!creds.complete())
            return;
        req.setRawHeader("Authorization",
                         "Token " + (creds.user.trimmed() + u':' + creds.secret)
                                        .toUtf8()
                                        .toBase64());
    }
};
} // namespace

namespace EngineInternal {
const BooruEngineOps *szurubooruOps() {
    static const EngineSzurubooru ops;
    return &ops;
}
} // namespace EngineInternal

namespace RemotePosts {
Post fromSzuru(const QJsonObject &o, const QString &baseUrl) {
    // Szurubooru /api/posts/ object. Field names defensive: instances vary.
    Post p;
    p.id = BooruEngineOps::jsonInt(o.value(QStringLiteral("id")));
    p.score = BooruEngineOps::jsonInt32(o.value(QStringLiteral("score")));
    const QString safety = o.value(QStringLiteral("safety")).toString().toLower();
    if (safety == QStringLiteral("unsafe"))
        p.rating = QStringLiteral("explicit");
    else if (safety == QStringLiteral("sketchy") ||
             safety == QStringLiteral("questionable"))
        p.rating = QStringLiteral("questionable");
    else if (safety == QStringLiteral("sensitive"))
        p.rating = QStringLiteral("sensitive");
    else
        p.rating = QStringLiteral("general");
    SzuruSplit split;
    for (const QJsonValue &tv : o.value(QStringLiteral("tags")).toArray()) {
        if (!tv.isObject())
            continue;
        const QJsonObject t = tv.toObject();
        QString name;
        for (const QJsonValue &n : t.value(QStringLiteral("names")).toArray()) {
            if (n.isString() && !n.toString().isEmpty()) {
                name = n.toString();
                break;
            }
        }
        name = name.trimmed();
        if (name.isEmpty() || !tagTokenOk(name))
            continue;
        szuruPlace(split, name,
                   t.value(QStringLiteral("category")).toString().toLower());
    }
    p.tagsArtist = split.artist.join(u' ');
    p.tagsCharacter = split.character.join(u' ');
    p.tagsCopyright = split.copyright.join(u' ');
    p.tagsGeneral = split.general.join(u' ');
    p.tagsMeta = split.meta.join(u' ');
    p.width = qMax(0, BooruEngineOps::jsonInt32(o.value(QStringLiteral("canvasWidth"))));
    if (!p.width)
        p.width = qMax(0, BooruEngineOps::jsonInt32(o.value(QStringLiteral("width"))));
    p.height = qMax(0, BooruEngineOps::jsonInt32(o.value(QStringLiteral("canvasHeight"))));
    if (!p.height)
        p.height = qMax(0, BooruEngineOps::jsonInt32(o.value(QStringLiteral("height"))));
    p.source = o.value(QStringLiteral("source")).toString();
    p.date = o.value(QStringLiteral("creationTime")).toString();
    auto abs = [&](const QString &u) {
        if (u.isEmpty() || u.startsWith(QStringLiteral("http")))
            return u;
        return baseUrl + (u.startsWith(u'/') ? u : u'/' + u);
    };
    // Field names vary by instance version: contentUrl (new) vs imageUrl.
    p.fileUrl = abs(o.value(QStringLiteral("contentUrl")).toString());
    if (p.fileUrl.isEmpty())
        p.fileUrl = abs(o.value(QStringLiteral("imageUrl")).toString());
    p.sampleUrl = abs(o.value(QStringLiteral("sampleUrl")).toString());
    if (p.sampleUrl.isEmpty())
        p.sampleUrl = p.fileUrl;
    p.previewUrl = abs(o.value(QStringLiteral("thumbnailUrl")).toString());
    if (p.previewUrl.isEmpty())
        p.previewUrl = abs(o.value(QStringLiteral("customThumbnailUrl")).toString());
    p.ext = BooruEngineOps::extOf(p.fileUrl);
    p.postUrl = postPageUrl(QStringLiteral("szurubooru"), baseUrl, p.id);
    BooruEngineOps::finishRemote(p, QStringLiteral("Szurubooru"), o, BooruEngine::Szurubooru);
    // Native custom categories survive alongside the fixed buckets.
    p.customTags = split.custom;
    for (auto it = split.custom.begin(); it != split.custom.end(); ++it)
        p.tags += u' ' + it.value().join(u' ');
    p.tags = p.tags.simplified();
    return p;
}
} // namespace RemotePosts
