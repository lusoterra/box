// Nuuru engine (Soybooru's proprietary booru; soybooru.com is the only
// known instance). Shapes reverse-engineered from the served HTML/JS
// (server-rendered search embed + client bundle endpoint map):
//   GET {base}/api/booru/posts?page= (1-indexed) &pageSize [&q=query]
//     -> {posts: [...], totalCount, totalPages}
//   GET {base}/api/booru/posts/<id> -> single post object
//   GET {base}/api/booru/tags/search?query=&limit= -> tag suggestions
//   media: {base}/api/booru/posts/<id>/file (full),
//          {base}/api/booru/posts/<id>/thumbnail
//   post pages: {base}/post/view/<id>
// Plain QNAM fetches hit a PoW wall ({"error":"pow_required"}) stacked on
// Cloudflare, so traffic must ride the browser-backed stack: the entry
// wants the powWall quirk (cookie capture + browser fetch, like ATF),
// never the plain stack alone.
// Tags carry native categories (Flag/Media/Meta/Series/Subvariant/Trend/
// Variant/Uncategorized + null); obvious ones fold into buckets
// (Series->copyright, Variant/Subvariant->character, Uncategorized/null->
// general), the rest survive verbatim in customTags (Szurubooru-style).
// Rating values beyond "safe" were never observed live; the mapping
// below assumes danbooru-style words and defaults to general.
#include "booruengine.h"
#include "explicittags.h"

#include <QFileInfo>
#include <QUrlQuery>

namespace {
// mimeType -> ext (post shape carries no separate extension field).
QString nuuruExt(const QJsonObject &o) {
    const QString mime =
        o.value(QStringLiteral("mimeType")).toString().toLower();
    if (mime == QStringLiteral("image/jpeg"))
        return QStringLiteral("jpg");
    if (mime == QStringLiteral("image/png"))
        return QStringLiteral("png");
    if (mime == QStringLiteral("image/gif"))
        return QStringLiteral("gif");
    if (mime == QStringLiteral("image/webp"))
        return QStringLiteral("webp");
    if (mime == QStringLiteral("video/mp4"))
        return QStringLiteral("mp4");
    if (mime == QStringLiteral("video/webm"))
        return QStringLiteral("webm");
    const QString fromName =
        QFileInfo(o.value(QStringLiteral("originalFileName")).toString())
            .suffix()
            .toLower();
    return fromName.isEmpty() ? QStringLiteral("jpg") : fromName;
}

QString nuuruRating(const QJsonObject &o) {
    const QString r = o.value(QStringLiteral("rating")).toString().toLower();
    if (r == QStringLiteral("explicit"))
        return QStringLiteral("explicit");
    if (r == QStringLiteral("questionable") || r == QStringLiteral("suggestive"))
        return QStringLiteral("questionable");
    if (r == QStringLiteral("sensitive"))
        return QStringLiteral("sensitive");
    return QStringLiteral("general");
}

class EngineNuuru : public BooruEngineOps {
public:
    QUrl searchUrl(const Context &ctx, const QStringList &tags, int page,
                   int limit) const override {
        QUrl url(ctx.baseUrl + QStringLiteral("/api/booru/posts"));
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("page"),
                           QString::number(qMax(1, page)));
        query.addQueryItem(QStringLiteral("pageSize"),
                           QString::number(qBound(1, limit, 100)));
        const QString q = joinTags(tags);
        if (!q.isEmpty())
            query.addQueryItem(QStringLiteral("q"), q);
        url.setQuery(query);
        return url;
    }
    SearchResult parseSearch(const QString &siteKey, const Context &ctx,
                             const QByteArray &body) const override {
        SearchResult result;
        result.site = siteKey;
        const QJsonDocument doc = parseJsonBody(body);
        if (doc.isNull() || !doc.isObject() ||
            !doc.object().value(QStringLiteral("posts")).isArray()) {
            result.error = doc.isNull() ? QStringLiteral("Unparseable response")
                                        : QStringLiteral("Unexpected response shape");
            return result;
        }
        for (const QJsonValue &v :
             doc.object().value(QStringLiteral("posts")).toArray()) {
            if (v.isObject()) {
                Post p = RemotePosts::fromNuuruSummary(v.toObject(),
                                                       ctx.baseUrl);
                p.siteKey = siteKey; // refresh targets resolve exactly
                result.posts.append(std::move(p));
            }
        }
        return result;
    }
    QUrl refreshUrl(const Context &ctx, qint64 id) const override {
        return QUrl(ctx.baseUrl + QStringLiteral("/api/booru/posts/") +
                    QString::number(id));
    }
    QJsonObject parseSingle(const QByteArray &body, bool &ok) const override {
        ok = false;
        const QJsonDocument doc = parseJsonBody(body);
        if (doc.isNull() || !doc.isObject())
            return {};
        ok = true;
        return doc.object();
    }
    void applyAuth(QNetworkRequest &req, const SiteCreds &creds) const override {
        Q_UNUSED(req);
        Q_UNUSED(creds);
        // Anonymous (logins ride cookies captured in the browser).
    }
};
} // namespace

namespace EngineInternal {
const BooruEngineOps *nuuruOps() {
    static const EngineNuuru ops;
    return &ops;
}
} // namespace EngineInternal

namespace RemotePosts {
static void nuuruPlace(const QString &name, const QString &ns, QString &artist,
                       QString &character, QString &copyright,
                       QString &general, QString &meta,
                       QMap<QString, QStringList> &custom) {
    QString bare = name.trimmed();
    if (bare.isEmpty() || !tagTokenOk(bare))
        return;
    const QString key = ns.isEmpty() ? QStringLiteral("uncategorized") : ns;
    custom[key].append(bare); // native taxonomy survives verbatim
    QString *field = &general;
    if (ns == QStringLiteral("character") || ns == QStringLiteral("variant") ||
        ns == QStringLiteral("subvariant"))
        field = &character;
    else if (ns == QStringLiteral("series"))
        field = &copyright;
    else if (ns == QStringLiteral("artist"))
        field = &artist;
    else if (ns == QStringLiteral("meta") || ns == QStringLiteral("medium") ||
             ns == QStringLiteral("title"))
        field = &meta;
    if (!field->isEmpty())
        *field += u' ';
    *field += bare;
}

static void nuuruFillTags(Post &p, const QJsonObject &o) {
    QString artist, character, copyright, general, meta;
    QMap<QString, QStringList> custom;
    const QJsonValue tags = o.value(QStringLiteral("tags"));
    if (tags.isArray()) {
        for (const QJsonValue &tv : tags.toArray()) {
            if (!tv.isObject())
                continue;
            const QJsonObject t = tv.toObject();
            nuuruPlace(t.value(QStringLiteral("name")).toString(),
                       t.value(QStringLiteral("category")).toString().toLower(),
                       artist, character, copyright, general, meta, custom);
        }
    }
    p.tagsArtist = artist;
    p.tagsCharacter = character;
    p.tagsCopyright = copyright;
    p.tagsGeneral = general;
    p.tagsMeta = meta;
    p.customTags = custom;
}

static void nuuruFillCommon(Post &p, const QJsonObject &o,
                            const QString &baseUrl) {
    p.id = BooruEngineOps::jsonInt(o.value(QStringLiteral("id")));
    p.ext = nuuruExt(o);
    p.fileUrl = baseUrl + QStringLiteral("/api/booru/posts/") +
                QString::number(p.id) + QStringLiteral("/file");
    // No separate sample rendition: the viewer streams the full file.
    p.sampleUrl = p.fileUrl;
    p.previewUrl = baseUrl + QStringLiteral("/api/booru/posts/") +
                   QString::number(p.id) + QStringLiteral("/thumbnail");
    p.width = BooruEngineOps::jsonInt32(o.value(QStringLiteral("width")));
    p.height = BooruEngineOps::jsonInt32(o.value(QStringLiteral("height")));
    p.rating = nuuruRating(o);
    p.score = BooruEngineOps::jsonInt32(o.value(QStringLiteral("score")));
    const QJsonValue src = o.value(QStringLiteral("source"));
    if (src.isString() && !src.toString().isEmpty())
        p.source = src.toString();
    else if (src.isArray() && !src.toArray().isEmpty())
        p.source = src.toArray().first().toString();
    p.date = o.value(QStringLiteral("uploadedAt")).toString();
    p.postUrl = baseUrl + QStringLiteral("/post/view/") + QString::number(p.id);
}

Post fromNuuruSummary(const QJsonObject &o, const QString &baseUrl) {
    // Search shape carries everything (tags included, like the
    // server-rendered embed): a summary refreshes identically.
    // Copy the object to normalize the md5 key download naming reads.
    QJsonObject norm(o);
    const QString md5 = o.value(QStringLiteral("md5Hash")).toString();
    if (!md5.isEmpty())
        norm[QStringLiteral("md5")] = md5;
    Post p;
    nuuruFillCommon(p, o, baseUrl);
    nuuruFillTags(p, o);
    BooruEngineOps::finishRemote(p, QStringLiteral("Nuuru"), norm,
                                 BooruEngine::Nuuru);
    return p;
}

Post fromNuuru(const QJsonObject &o, const QString &baseUrl) {
    // Single-post shape matches the search objects.
    QJsonObject norm(o);
    const QString md5 = o.value(QStringLiteral("md5Hash")).toString();
    if (!md5.isEmpty())
        norm[QStringLiteral("md5")] = md5;
    Post p;
    nuuruFillCommon(p, o, baseUrl);
    nuuruFillTags(p, o);
    BooruEngineOps::finishRemote(p, QStringLiteral("Nuuru"), norm,
                                 BooruEngine::Nuuru);
    return p;
}
} // namespace RemotePosts
