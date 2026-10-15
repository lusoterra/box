// Shimmie engine: danbooru_api ext, XML only (see
// ext/danbooru_api/main.php). No request auth.
#include "booruengine.h"
#include "explicittags.h"

#include <QUrlQuery>
#include <QXmlStreamReader>

namespace {
class EngineShimmie : public BooruEngineOps {
public:
    QUrl searchUrl(const Context &ctx, const QStringList &tags, int page,
                   int limit) const override {
        QUrl url(ctx.baseUrl + QStringLiteral("/api/danbooru/post/index.xml"));
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("tags"), joinTags(tags));
        query.addQueryItem(QStringLiteral("page"), QString::number(qMax(1, page)));
        query.addQueryItem(QStringLiteral("limit"), QString::number(qBound(1, limit, 100)));
        url.setQuery(query);
        return url;
    }
    SearchResult parseSearch(const QString &siteKey, const Context &ctx,
                             const QByteArray &body) const override {
        BooruSite eff;
        eff.key = siteKey;
        eff.baseUrl = ctx.baseUrl;
        eff.engine = BooruEngine::Shimmie;
        return RemotePosts::parseShimmieXml(eff, body);
    }
    QUrl refreshUrl(const Context &ctx, qint64 id) const override {
        QUrl u(ctx.baseUrl + QStringLiteral("/api/danbooru/post/index.xml"));
        QUrlQuery q;
        q.addQueryItem(QStringLiteral("id"), QString::number(id));
        q.addQueryItem(QStringLiteral("limit"), QStringLiteral("1"));
        u.setQuery(q);
        return u;
    }
    QJsonObject parseSingle(const QByteArray &body, bool &ok) const override {
        ok = false;
        // XML shape: <posts><post id .../></posts> - take the first post.
        QXmlStreamReader xml(body);
        if (xml.readNextStartElement() && xml.name() == QLatin1String("posts")) {
            while (xml.readNextStartElement()) {
                if (xml.name() == QLatin1String("post")) {
                    QJsonObject o;
                    for (const QXmlStreamAttribute &a : xml.attributes()) {
                        const QString v = a.value().toString();
                        bool num = false;
                        const qint64 n = v.toLongLong(&num);
                        o.insert(a.name().toString(),
                                 num ? QJsonValue(n) : QJsonValue(v));
                    }
                    ok = true;
                    return o;
                }
                xml.skipCurrentElement();
            }
        }
        return {};
    }
    void applyAuth(QNetworkRequest &, const SiteCreds &) const override {
        // No auth scheme.
    }
};
} // namespace

namespace EngineInternal {
const BooruEngineOps *shimmieOps() {
    static const EngineShimmie ops;
    return &ops;
}
} // namespace EngineInternal

namespace RemotePosts {
SearchResult parseShimmieXml(const BooruSite &site, const QByteArray &body) {
    SearchResult result;
    result.site = site.key;
    QXmlStreamReader xml(body);
    if (xml.readNextStartElement() && xml.name() == QLatin1String("posts")) {
        while (xml.readNextStartElement()) {
            if (xml.name() == QLatin1String("post")) {
                QMap<QString, QString> attrs;
                for (const QXmlStreamAttribute &a : xml.attributes())
                    attrs.insert(a.name().toString(), a.value().toString());
                xml.skipCurrentElement();
                result.posts.append(fromShimmie(attrs, site.baseUrl));
            } else {
                xml.skipCurrentElement();
            }
        }
    }
    if (xml.hasError()) {
        result.error = QStringLiteral("Unparseable response");
        result.posts.clear();
    }
    return result;
}

Post fromShimmie(const QMap<QString, QString> &attrs, const QString &baseUrl) {
    Post p;
    p.id = attrs.value(QStringLiteral("id")).toLongLong();
    p.score = attrs.value(QStringLiteral("score")).toInt();
    p.rating = normalizeRating(attrs.value(QStringLiteral("rating"))); // "?" -> general
    p.tagsGeneral = attrs.value(QStringLiteral("tags"));
    p.untyped = p.tagsGeneral; // flat blob: classifiable later
    p.width = attrs.value(QStringLiteral("width")).toInt();
    p.height = attrs.value(QStringLiteral("height")).toInt();
    p.source = attrs.value(QStringLiteral("source"));
    p.date = attrs.value(QStringLiteral("date"));
    auto abs = [&](const QString &u) {
        if (u.isEmpty() || u.startsWith(QStringLiteral("http")))
            return u;
        return baseUrl + (u.startsWith(u'/') ? u : u'/' + u);
    };
    p.fileUrl = abs(attrs.value(QStringLiteral("file_url")));
    p.previewUrl = abs(attrs.value(QStringLiteral("preview_url")));
    p.sampleUrl = p.previewUrl;
    p.ext = BooruEngineOps::extOf(p.fileUrl);
    const QString md5 = attrs.value(QStringLiteral("md5"));
    p.postUrl = postPageUrl(QStringLiteral("shimmie"), baseUrl, p.id);
    QJsonObject raw;
    raw[QStringLiteral("id")] = p.id;
    raw[QStringLiteral("tags")] = p.tagsGeneral;
    raw[QStringLiteral("score")] = p.score;
    raw[QStringLiteral("rating")] = attrs.value(QStringLiteral("rating"));
    raw[QStringLiteral("width")] = p.width;
    raw[QStringLiteral("height")] = p.height;
    raw[QStringLiteral("source")] = p.source;
    raw[QStringLiteral("file_url")] = p.fileUrl;
    raw[QStringLiteral("preview_url")] = p.previewUrl;
    raw[QStringLiteral("md5")] = md5;
    raw[QStringLiteral("created_at")] = p.date;
    BooruEngineOps::finishRemote(p, QStringLiteral("Shimmie"), raw, BooruEngine::Shimmie);
    return p;
}
} // namespace RemotePosts
