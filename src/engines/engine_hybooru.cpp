// HyBooru engine: GET /api/post?query=&page= (0-indexed) &pageSize
// (+hashes/blurhash flags) listing, GET /api/post/<id> detail, Token-less
// anonymous access. Shapes verified against a live instance (funmaker).
//
// Gotchas (documented, not worked around):
// - Summaries carry NO tags (and no dimensions): the grid/sidebar show
//   media without tags; the viewer lazy-fetches the detail object on open
//   (tags land via the normal refresh path). Favourites taken from a
//   summary stay tagless until viewed.
// - rating is a 0-1 STAR score, not a content rating: every post maps to
//   "general". Server-side rating: filters are stripped (they would
//   mis-filter as star searches); the client rating box still filters.
// - File URLs are conventional, not API-provided: /files/f{sha256}{ext}
//   (full) and /files/t{sha256}.thumbnail. Post pages live at /posts/<id>.
#include "booruengine.h"
#include "explicittags.h"

#include <QUrlQuery>

namespace {
// HyBooru mime codes (Hydrus filetype enum): stills vs motion.
bool hybooruVideoMime(int mime) {
    switch (mime) {
    case 9:  // VIDEO_FLV
    case 14: // VIDEO_MP4
    case 18: // VIDEO_WMV
    case 20: // VIDEO_MKV
    case 21: // VIDEO_WEBM
    case 25: // VIDEO_MPEG
    case 26: // VIDEO_MOV
    case 27: // VIDEO_AVI
        return true;
    default:
        return false;
    }
}

// The API spells extensions with a leading dot (".jpg").
QString hybooruExt(const QJsonObject &o, int mime) {
    QString ext = o.value(QStringLiteral("extension")).toString().toLower();
    if (ext.startsWith(u'.'))
        ext = ext.mid(1);
    // Leading alphanumerics only (same traversal rule as extOf).
    QString clean;
    for (const QChar &c : ext) {
        if (c.isLetterOrNumber())
            clean += c;
        else
            break;
    }
    if (!clean.isEmpty())
        return clean;
    if (hybooruVideoMime(mime))
        return QStringLiteral("mp4");
    return QStringLiteral("jpg");
}

QString hybooruFileUrl(const QString &baseUrl, const QString &sha256,
                       const QString &ext) {
    if (sha256.isEmpty() || ext.isEmpty())
        return {};
    return baseUrl + QStringLiteral("/files/f") + sha256 + u'.' + ext;
}

class EngineHybooru : public BooruEngineOps {
public:
    QUrl searchUrl(const Context &ctx, const QStringList &tags, int page,
                   int limit) const override {
        QUrl url(ctx.baseUrl + QStringLiteral("/api/post"));
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("query"), joinTags(tags));
        // box pages are 1-indexed; the API is 0-indexed.
        query.addQueryItem(QStringLiteral("page"),
                           QString::number(qMax(0, page - 1)));
        query.addQueryItem(QStringLiteral("pageSize"),
                           QString::number(qBound(1, limit, 100)));
        // md5 only arrives with hashes=true (naming + sidecars need it).
        query.addQueryItem(QStringLiteral("hashes"), QStringLiteral("true"));
        query.addQueryItem(QStringLiteral("blurhash"), QStringLiteral("true"));
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
                Post p = RemotePosts::fromHybooruSummary(v.toObject(),
                                                         ctx.baseUrl);
                p.siteKey = siteKey; // refresh targets resolve exactly
                result.posts.append(std::move(p));
            }
        }
        return result;
    }
    QUrl refreshUrl(const Context &ctx, qint64 id) const override {
        return QUrl(ctx.baseUrl + QStringLiteral("/api/post/") +
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
        // Anonymous: instances take no keys (yet).
    }
};
} // namespace

namespace EngineInternal {
const BooruEngineOps *hybooruOps() {
    static const EngineHybooru ops;
    return &ops;
}
} // namespace EngineInternal

namespace RemotePosts {
// Namespace -> box bucket. Plain tags land in general; medium:/meta:/
// title: describe the post rather than its subject, so they read as meta.
static QString hybooruBucket(const QString &ns) {
    if (ns == QStringLiteral("character"))
        return QStringLiteral("character");
    if (ns == QStringLiteral("creator"))
        return QStringLiteral("artist");
    if (ns == QStringLiteral("series"))
        return QStringLiteral("copyright");
    if (ns == QStringLiteral("medium") || ns == QStringLiteral("meta") ||
        ns == QStringLiteral("title"))
        return QStringLiteral("meta");
    return QStringLiteral("general");
}

static void hybooruPlace(const QString &name, QString &artist,
                         QString &character, QString &copyright,
                         QString &general, QString &meta,
                         QMap<QString, QStringList> &custom) {
    QString tag = name.trimmed();
    if (tag.isEmpty() || !tagTokenOk(tag))
        return;
    QString ns;
    QString bare = tag;
    const int colon = tag.indexOf(u':');
    if (colon > 0) {
        ns = tag.left(colon).toLower();
        bare = tag.mid(colon + 1).trimmed();
        if (bare.isEmpty() || !tagTokenOk(bare))
            return;
        custom[ns].append(bare); // native taxonomy survives verbatim
    }
    const QString bucket = hybooruBucket(ns);
    QString *field = &general;
    if (bucket == QStringLiteral("artist"))
        field = &artist;
    else if (bucket == QStringLiteral("character"))
        field = &character;
    else if (bucket == QStringLiteral("copyright"))
        field = &copyright;
    else if (bucket == QStringLiteral("meta"))
        field = &meta;
    if (!field->isEmpty())
        *field += u' ';
    *field += bare;
}

static void hybooruFillTags(Post &p, const QJsonObject &o) {
    QString artist, character, copyright, general, meta;
    QMap<QString, QStringList> custom;
    const QJsonValue tags = o.value(QStringLiteral("tags"));
    if (tags.isObject()) {
        // Detail shape: {"name": usage-count}.
        const QJsonObject tagObj = tags.toObject();
        for (auto it = tagObj.begin(); it != tagObj.end(); ++it)
            hybooruPlace(it.key(), artist, character, copyright, general,
                         meta, custom);
    }
    p.tagsArtist = artist;
    p.tagsCharacter = character;
    p.tagsCopyright = copyright;
    p.tagsGeneral = general;
    p.tagsMeta = meta;
    p.customTags = custom;
}

static void hybooruFillCommon(Post &p, const QJsonObject &o,
                              const QString &baseUrl) {
    p.id = BooruEngineOps::jsonInt(o.value(QStringLiteral("id")));
    const QString sha256 = o.value(QStringLiteral("sha256")).toString();
    const int mime = BooruEngineOps::jsonInt32(o.value(QStringLiteral("mime")));
    const QString ext = hybooruExt(o, mime);
    p.ext = ext;
    p.fileUrl = hybooruFileUrl(baseUrl, sha256, ext);
    // No separate sample rendition: the viewer streams the full file.
    p.sampleUrl = p.fileUrl;
    if (!sha256.isEmpty())
        p.previewUrl = baseUrl + QStringLiteral("/files/t") + sha256 +
                       QStringLiteral(".thumbnail");
    p.width = BooruEngineOps::jsonInt32(o.value(QStringLiteral("width")));
    p.height = BooruEngineOps::jsonInt32(o.value(QStringLiteral("height")));
    // Star score, not a content rating: everything reads "general" (the
    // explicit-rating healer may still upgrade from tags).
    p.rating = QStringLiteral("general");
    for (const QJsonValue &s : o.value(QStringLiteral("sources")).toArray()) {
        if (s.isString() && !s.toString().isEmpty()) {
            p.source = s.toString();
            break;
        }
    }
    p.date = o.value(QStringLiteral("posted")).toString();
    p.postUrl = baseUrl + QStringLiteral("/posts/") + QString::number(p.id);
}

Post fromHybooruSummary(const QJsonObject &o, const QString &baseUrl) {
    // Search shape: id/sha256/md5?/size/mime/posted/extension. No tags,
    // no dimensions — the viewer lazy-fetches the detail object on open.
    Post p;
    hybooruFillCommon(p, o, baseUrl);
    BooruEngineOps::finishRemote(p, QStringLiteral("HyBooru"), o,
                                 BooruEngine::Hybooru);
    return p;
}

Post fromHybooru(const QJsonObject &o, const QString &baseUrl) {
    // Detail shape: summary fields plus tags:{name:usage}, sources[],
    // width/height, rating (stars), duration.
    Post p;
    hybooruFillCommon(p, o, baseUrl);
    hybooruFillTags(p, o);
    BooruEngineOps::finishRemote(p, QStringLiteral("HyBooru"), o,
                                 BooruEngine::Hybooru);
    return p;
}
} // namespace RemotePosts
