// Moebooru engine (Yandere / Konachan): /post.json listing, Basic auth.
#include "booruengine.h"
#include "explicittags.h"

#include <QDateTime>
#include <QTimeZone>
#include <QUrlQuery>

namespace {
class EngineMoebooru : public BooruEngineOps {
public:
    QUrl searchUrl(const Context &ctx, const QStringList &tags, int page,
                   int limit) const override {
        QUrl url(ctx.baseUrl + QStringLiteral("/post.json"));
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("tags"), joinTags(tags));
        query.addQueryItem(QStringLiteral("page"), QString::number(qMax(1, page)));
        query.addQueryItem(QStringLiteral("limit"), QString::number(qBound(1, limit, 100)));
        url.setQuery(query);
        return url;
    }
    SearchResult parseSearch(const QString &siteKey, const Context &ctx,
                             const QByteArray &body) const override {
        SearchResult result;
        result.site = siteKey;
        const QJsonDocument doc = parseJsonBody(body);
        if (doc.isNull() || !doc.isArray()) {
            result.error = doc.isNull() ? QStringLiteral("Unparseable response")
                                        : QStringLiteral("Unexpected response shape");
            return result;
        }
        for (const QJsonValue &v : doc.array()) {
            if (v.isObject())
                result.posts.append(
                    RemotePosts::fromMoebooru(v.toObject(), ctx.baseUrl));
        }
        return result;
    }
    QUrl refreshUrl(const Context &ctx, qint64 id) const override {
        QUrl u(ctx.baseUrl + QStringLiteral("/post.json"));
        QUrlQuery q;
        q.addQueryItem(QStringLiteral("tags"), QStringLiteral("id:") + QString::number(id));
        q.addQueryItem(QStringLiteral("limit"), QStringLiteral("1"));
        u.setQuery(q);
        return u;
    }
    QJsonObject parseSingle(const QByteArray &body, bool &ok) const override {
        ok = false;
        const QJsonDocument doc = parseJsonBody(body);
        if (doc.isNull())
            return {};
        return firstOfArray(doc.array(), ok);
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
const BooruEngineOps *moebooruOps() {
    static const EngineMoebooru ops;
    return &ops;
}
} // namespace EngineInternal

namespace RemotePosts {
Post fromMoebooru(const QJsonObject &o, const QString &baseUrl) {
    Post p;
    p.id = BooruEngineOps::jsonInt(o.value(QStringLiteral("id")));
    p.score = static_cast<int>(BooruEngineOps::jsonInt(o.value(QStringLiteral("score"))));
    p.rating = normalizeRating(o.value(QStringLiteral("rating")).toString());
    p.tagsGeneral = o.value(QStringLiteral("tags")).toString();
    p.untyped = p.tagsGeneral;
    p.width = static_cast<int>(BooruEngineOps::jsonInt(o.value(QStringLiteral("width"))));
    p.height = static_cast<int>(BooruEngineOps::jsonInt(o.value(QStringLiteral("height"))));
    p.source = o.value(QStringLiteral("source")).toString();
    const QJsonValue created = o.value(QStringLiteral("created_at"));
    if (created.isDouble())
        p.date = QDateTime::fromSecsSinceEpoch(static_cast<qint64>(created.toDouble()),
                                               QTimeZone::UTC)
                     .toString(Qt::ISODate);
    else
        p.date = created.toString();
    p.fileUrl = o.value(QStringLiteral("file_url")).toString();
    p.sampleUrl = o.value(QStringLiteral("sample_url")).toString();
    p.previewUrl = o.value(QStringLiteral("preview_url")).toString();
    p.ext = BooruEngineOps::extOf(p.fileUrl, o.value(QStringLiteral("file_ext")).toString());
    const QString site = baseUrl.contains(QStringLiteral("konachan"))
                             ? QStringLiteral("Konachan")
                             : QStringLiteral("Yandere");
    const QString key = site == QStringLiteral("Konachan") ? QStringLiteral("konachan")
                                                           : QStringLiteral("yandere");
    p.postUrl = postPageUrl(key, baseUrl, p.id);
    BooruEngineOps::finishRemote(p, site, o, BooruEngine::Moebooru);
    return p;
}
} // namespace RemotePosts
