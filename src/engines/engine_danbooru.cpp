// Danbooru engine: /posts.json listing, object-shaped refresh, Basic auth.
#include "booruengine.h"
#include "explicittags.h"

#include <QUrlQuery>

namespace {
class EngineDanbooru : public BooruEngineOps {
public:
    QUrl searchUrl(const Context &ctx, const QStringList &tags, int page,
                   int limit) const override {
        QUrl url(ctx.baseUrl + QStringLiteral("/posts.json"));
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("tags"), joinTags(tags));
        query.addQueryItem(QStringLiteral("page"), QString::number(qMax(1, page)));
        query.addQueryItem(QStringLiteral("limit"), QString::number(qBound(1, limit, 200)));
        url.setQuery(query);
        return url;
    }
    SearchResult parseSearch(const QString &siteKey, const Context &ctx,
                             const QByteArray &body) const override {
        SearchResult result;
        result.site = siteKey;
        const QJsonDocument doc = parseJsonBody(body);
        if (doc.isNull() || !doc.isArray()) {
            // Gelbooru dapi sometimes answers XML even with json=1.
            result.error = doc.isNull() ? QStringLiteral("Unparseable response")
                                        : QStringLiteral("Unexpected response shape");
            return result;
        }
        for (const QJsonValue &v : doc.array()) {
            if (v.isObject())
                result.posts.append(
                    RemotePosts::fromDanbooru(v.toObject(), ctx.baseUrl));
        }
        return result;
    }
    QUrl refreshUrl(const Context &ctx, qint64 id) const override {
        return QUrl(ctx.baseUrl + QStringLiteral("/posts/") + QString::number(id) +
                    QStringLiteral(".json"));
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
        if (!creds.complete())
            return;
        const QByteArray token =
            (creds.user.trimmed() + u':' + creds.secret).toUtf8().toBase64();
        req.setRawHeader("Authorization", "Basic " + token);
    }
};
} // namespace

namespace EngineInternal {
const BooruEngineOps *danbooruOps() {
    static const EngineDanbooru ops;
    return &ops;
}
} // namespace EngineInternal

namespace RemotePosts {
Post fromDanbooru(const QJsonObject &o, const QString &baseUrl) {
    Post p;
    p.id = BooruEngineOps::jsonInt(o.value(QStringLiteral("id")));
    QJsonValue scoreV = o.value(QStringLiteral("score"));
    if (scoreV.isObject())
        scoreV = scoreV.toObject().value(QStringLiteral("total"));
    p.score = BooruEngineOps::jsonInt32(scoreV);
    p.rating = normalizeRating(o.value(QStringLiteral("rating")).toString());
    p.tagsArtist = o.value(QStringLiteral("tag_string_artist")).toString();
    p.tagsCharacter = o.value(QStringLiteral("tag_string_character")).toString();
    p.tagsCopyright = o.value(QStringLiteral("tag_string_copyright")).toString();
    p.tagsGeneral = o.value(QStringLiteral("tag_string_general")).toString();
    if (p.tagsGeneral.isEmpty())
        p.tagsGeneral = o.value(QStringLiteral("tag_string")).toString();
    p.tagsMeta = o.value(QStringLiteral("tag_string_meta")).toString();
    p.width = qMax(0, BooruEngineOps::jsonInt32(o.value(QStringLiteral("image_width"))));
    p.height = qMax(0, BooruEngineOps::jsonInt32(o.value(QStringLiteral("image_height"))));
    p.source = o.value(QStringLiteral("source")).toString();
    p.date = o.value(QStringLiteral("created_at")).toString();
    p.fileUrl = o.value(QStringLiteral("file_url")).toString();
    if (p.fileUrl.isEmpty())
        p.fileUrl = o.value(QStringLiteral("large_file_url")).toString();
    p.sampleUrl = o.value(QStringLiteral("large_file_url")).toString();
    p.previewUrl = o.value(QStringLiteral("preview_file_url")).toString();
    p.ext = BooruEngineOps::extOf(p.fileUrl, o.value(QStringLiteral("file_ext")).toString());
    p.postUrl = baseUrl + QStringLiteral("/posts/") + QString::number(p.id);
    BooruEngineOps::finishRemote(p, QStringLiteral("Danbooru"), o, BooruEngine::Danbooru);
    return p;
}
} // namespace RemotePosts
