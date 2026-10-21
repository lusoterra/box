// HiddenBooru engine (single Tor-first instance + I2P mirror; shapes
// reverse-engineered from the v0.8 source tarball in the repo, before it
// is deleted):
//   GET /api/posts?terms=&page= (1-indexed) &limit&order=ASC|DESC
//     -> array of post objects (NO wrapper, NO total).
//   GET /api/tag/complete?term= -> flat array of tag name strings.
//   No comment list endpoint (create/delete only).
// A post carries id/hash/htype/mtype/fmt/size/width/height/rating
// (letter u/s/q/e or full name)/tags (space-separated)/file_url +
// preview_url (RELATIVE — resolve against base)/source_url.
// Single-post refresh reuses the search endpoint with terms=id:<id>.
// Plain fetches must ride the Tor browser/proxy stack; prefer the onion
// URL — I2P is extremely slow. Default engine label: "HiddenBooru".
#include "booruengine.h"
#include "explicittags.h"

#include <QUrlQuery>

namespace {
QString hiddenbooruRating(const QJsonObject &o) {
    // Rating letters per the tarball field docs: u = unrated, s = safe,
    // q = questionable, e = explicit (full names tolerated too).
    const QString r = o.value(QStringLiteral("rating")).toString().toLower();
    const QString v = (r == QStringLiteral("u") || r == QStringLiteral("unrated"))
                          ? QStringLiteral("general")
                      : (r == QStringLiteral("s") || r == QStringLiteral("safe") ||
                         r == QStringLiteral("general"))
                          ? QStringLiteral("general")
                      : (r == QStringLiteral("q") ||
                         r == QStringLiteral("questionable"))
                          ? QStringLiteral("questionable")
                      : (r == QStringLiteral("e") || r == QStringLiteral("explicit"))
                          ? QStringLiteral("explicit")
                      : (r == QStringLiteral("sensitive"))
                          ? QStringLiteral("sensitive")
                          : QStringLiteral("general");
    return v;
}

bool hiddenbooruIsVideo(const QJsonObject &o, const QString &ext) {
    if (o.value(QStringLiteral("mtype")).toString().compare(
            QStringLiteral("v"), Qt::CaseInsensitive) == 0)
        return true;
    return ext == QStringLiteral("mp4") || ext == QStringLiteral("webm") ||
           ext == QStringLiteral("mkv") || ext == QStringLiteral("mov") ||
           ext == QStringLiteral("avi") || ext == QStringLiteral("flv") ||
           ext == QStringLiteral("wmv") || ext == QStringLiteral("mpeg");
}

class EngineHiddenbooru : public BooruEngineOps {
public:
    QUrl searchUrl(const Context &ctx, const QStringList &tags, int page,
                   int limit) const override {
        QUrl url(ctx.baseUrl + QStringLiteral("/api/posts"));
        QUrlQuery query;
        const QString q = joinTags(tags);
        if (!q.isEmpty())
            query.addQueryItem(QStringLiteral("terms"), q);
        query.addQueryItem(QStringLiteral("page"),
                           QString::number(qMax(1, page)));
        query.addQueryItem(QStringLiteral("limit"),
                           QString::number(qBound(1, limit, 100)));
        query.addQueryItem(QStringLiteral("order"), QStringLiteral("DESC"));
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
            if (v.isObject()) {
                Post p = RemotePosts::fromHiddenbooru(v.toObject(),
                                                      ctx.baseUrl);
                p.siteKey = siteKey; // refresh targets resolve exactly
                result.posts.append(std::move(p));
            }
        }
        return result;
    }
    QUrl refreshUrl(const Context &ctx, qint64 id) const override {
        // No single-post endpoint: id: terms narrow the search to one row.
        QUrl url(ctx.baseUrl + QStringLiteral("/api/posts"));
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("terms"),
                           QStringLiteral("id:") + QString::number(id));
        query.addQueryItem(QStringLiteral("page"), QStringLiteral("1"));
        query.addQueryItem(QStringLiteral("limit"), QStringLiteral("1"));
        url.setQuery(query);
        return url;
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
        return firstOfArray(doc.array(), ok); // search-shaped refresh
    }
    void applyAuth(QNetworkRequest &req, const SiteCreds &creds) const override {
        Q_UNUSED(req);
        Q_UNUSED(creds);
        // Anonymous (logins ride cookies captured in the browser).
    }
};
} // namespace

namespace EngineInternal {
const BooruEngineOps *hiddenbooruOps() {
    static const EngineHiddenbooru ops;
    return &ops;
}
} // namespace EngineInternal

namespace RemotePosts {
Post fromHiddenbooru(const QJsonObject &o, const QString &baseUrl) {
    Post p;
    p.id = BooruEngineOps::jsonInt(o.value(QStringLiteral("id")));
    QString ext = o.value(QStringLiteral("fmt")).toString().toLower();
    if (ext.isEmpty())
        ext = BooruEngineOps::extOf(
            o.value(QStringLiteral("file_url")).toString(),
            QStringLiteral("jpg"));
    p.ext = ext;
    // Relative file paths resolve straight against the base (no extra
    // prefix: unlike Safebooru dapi, these are already full paths).
    auto resolveRel = [&](const QString &u) {
        if (u.isEmpty() || u.startsWith(QStringLiteral("http")))
            return u;
        QString path = u;
        while (path.startsWith(u'/'))
            path.remove(0, 1);
        return baseUrl + u'/' + path;
    };
    p.fileUrl = resolveRel(o.value(QStringLiteral("file_url")).toString());
    p.sampleUrl = p.fileUrl; // no separate sample rendition
    p.previewUrl = resolveRel(o.value(QStringLiteral("preview_url")).toString());
    if (p.previewUrl.isEmpty())
        p.previewUrl = p.fileUrl;
    p.width = BooruEngineOps::jsonInt32(o.value(QStringLiteral("width")));
    p.height = BooruEngineOps::jsonInt32(o.value(QStringLiteral("height")));
    p.rating = hiddenbooruRating(o);
    p.score = BooruEngineOps::jsonInt32(o.value(QStringLiteral("score")));
    const QJsonValue src = o.value(QStringLiteral("source_url"));
    if (src.isString() && !src.toString().isEmpty())
        p.source = src.toString();
    p.date = o.value(QStringLiteral("created")).toString();
    if (p.date.isEmpty())
        p.date = o.value(QStringLiteral("changed")).toString();
    p.tagsGeneral = BooruEngineOps::joinTags(
        BooruEngineOps::splitTags(o.value(QStringLiteral("tags")).toString()));
    p.untyped = p.tagsGeneral;
    p.postUrl =
        baseUrl + QStringLiteral("/post/") + QString::number(p.id) +
        QStringLiteral("/view");
    QJsonObject raw(o);
    const QString hash = o.value(QStringLiteral("hash")).toString();
    bool hex32 = hash.size() == 32;
    for (const QChar &c : hash) {
        const ushort u = c.unicode();
        if (!((u >= u'0' && u <= u'9') || (u >= u'a' && u <= u'f') ||
              (u >= u'A' && u <= u'F'))) {
            hex32 = false;
            break;
        }
    }
    if (!hash.isEmpty() && (o.value(QStringLiteral("htype")).toString().compare(
                                QStringLiteral("md5"), Qt::CaseInsensitive) == 0 ||
                            hex32))
        raw[QStringLiteral("md5")] = hash.toLower();
    BooruEngineOps::finishRemote(p, QStringLiteral("HiddenBooru"), raw,
                                 BooruEngine::Hiddenbooru);
    return p;
}
} // namespace RemotePosts
