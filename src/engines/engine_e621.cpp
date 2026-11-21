// e621 engine (e621.net explicit + e926.net SFW mirror, same database).
// Shapes verified live 2026-11-21 (anonymous, no account needed):
//   GET {base}/posts.json?tags=...&page=N&limit=M -> bare JSON array
//   GET {base}/posts/<id>.json -> single post object
// Post shape (Danbooru fork, e621ng): files.{original{url,w,h},preview,
// sample.{jpg,webp},meta{md5,ext}}, stats.{score.total,fav_count,
// comment_count}, flags.{deleted,...}, has.{notes,sample,...},
// relationships.{parent_id,children[]}, rating s/q/e, sources[],
// description (DText), tags[] FLAT (no per-category splits in either
// shape: buckets come from the shared classifier waterfall, species/lore
// stay native-capable through customTags on a later pass).
// API rules (https://e621.net/help/api): descriptive non-empty
// User-Agent required (default library UAs are blocked, browser
// impersonation gets blocked); hard 2 req/s, aim <=1/s sustained;
// auth (login + api_key, Basic or query) only needed for private actions
// (votes/favorites) - search is public. Tag category ids (verified via
// /tags.json): 0 general, 1 artist, 3 copyright, 4 character, 5 species,
// 6 invalid, 7 meta, 8 lore (contributor is newer, unnumbered here).
#include "booruengine.h"
#include "engineinternal.h"
#include "explicittags.h"

#include <QUrlQuery>

namespace {

QString e621Rating(const QJsonValue &v) {
    const QString r = v.toString().toLower();
    if (r == QStringLiteral("e"))
        return QStringLiteral("explicit");
    if (r == QStringLiteral("q"))
        return QStringLiteral("questionable");
    // s and anything unknown: safe side of the filter.
    return QStringLiteral("general");
}

class EngineE621 : public BooruEngineOps {
public:
    QUrl searchUrl(const Context &ctx, const QStringList &tags, int page,
                   int limit) const override {
        QUrl url(RemotePosts::apiBaseUrl(ctx.engine, ctx.baseUrl) +
                 QStringLiteral("/posts.json"));
        QUrlQuery query;
        const QString q = joinTags(tags);
        if (!q.isEmpty())
            query.addQueryItem(QStringLiteral("tags"), q);
        query.addQueryItem(QStringLiteral("page"), QString::number(qMax(1, page)));
        query.addQueryItem(QStringLiteral("limit"),
                           QString::number(qBound(1, limit, 320)));
        if (ctx.creds.complete()) {
            query.addQueryItem(QStringLiteral("login"),
                               ctx.creds.user.trimmed());
            query.addQueryItem(QStringLiteral("api_key"), ctx.creds.secret);
        }
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
            if (!v.isObject())
                continue;
            Post p = RemotePosts::fromE621(v.toObject(), ctx.baseUrl);
            if (p.id == 0 || p.fileUrl.isEmpty())
                continue; // deleted/fileless rows carry no renditions
            p.siteKey = siteKey; // refresh targets resolve exactly
            result.posts.append(std::move(p));
        }
        return result;
    }
    QUrl refreshUrl(const Context &ctx, qint64 id) const override {
        return QUrl(RemotePosts::apiBaseUrl(ctx.engine, ctx.baseUrl) +
                    QStringLiteral("/posts/") + QString::number(id) +
                    QStringLiteral(".json"));
    }
    QJsonObject parseSingle(const QByteArray &body, bool &ok) const override {
        ok = false;
        const QJsonDocument doc = parseJsonBody(body);
        if (doc.isNull())
            return {};
        if (doc.isObject() && doc.object().contains(QStringLiteral("id"))) {
            ok = true;
            return doc.object();
        }
        return firstOfArray(doc.array(), ok); // tolerate list shape anyway
    }
    void applyAuth(QNetworkRequest &req, const SiteCreds &creds) const override {
        if (!creds.complete())
            return;
        // Same scheme as Danbooru (e621 accepts Basic or query params;
        // the query params are already on search URLs, this covers
        // refresh/detail fetches).
        const QByteArray token =
            (creds.user.trimmed() + u':' + creds.secret).toUtf8().toBase64();
        req.setRawHeader("Authorization", "Basic " + token);
    }
};

} // namespace

namespace EngineInternal {
const BooruEngineOps *e621Ops() {
    static const EngineE621 ops;
    return &ops;
}
} // namespace EngineInternal

namespace RemotePosts {

void e621FillCommon(Post &p, const QJsonObject &o, const QString &baseUrl) {
    p.id = BooruEngineOps::jsonInt(o.value(QStringLiteral("id")));
    const QJsonObject files = o.value(QStringLiteral("files")).toObject();
    const QJsonObject meta = files.value(QStringLiteral("meta")).toObject();
    const QJsonObject original = files.value(QStringLiteral("original")).toObject();
    const QJsonObject sample = files.value(QStringLiteral("sample")).toObject();
    const QJsonObject preview = files.value(QStringLiteral("preview")).toObject();
    p.ext = meta.value(QStringLiteral("ext")).toString().toLower();
    p.fileUrl = original.value(QStringLiteral("url")).toString();
    // Sample first (viewer prefers it); jpg renditions exist for every
    // post, webp only mirrors them.
    p.sampleUrl = sample.value(QStringLiteral("jpg")).toString();
    if (p.sampleUrl.isEmpty())
        p.sampleUrl = sample.value(QStringLiteral("webp")).toString();
    p.previewUrl = preview.value(QStringLiteral("jpg")).toString();
    if (p.previewUrl.isEmpty())
        p.previewUrl = preview.value(QStringLiteral("webp")).toString();
    p.width = BooruEngineOps::jsonInt32(original.value(QStringLiteral("width")));
    p.height = BooruEngineOps::jsonInt32(original.value(QStringLiteral("height")));
    p.rating = e621Rating(o.value(QStringLiteral("rating")));
    const QJsonObject stats =
        o.value(QStringLiteral("stats")).toObject();
    const QJsonObject score = stats.value(QStringLiteral("score")).toObject();
    p.score = BooruEngineOps::jsonInt32(score.value(QStringLiteral("total")));
    const QJsonArray sources = o.value(QStringLiteral("sources")).toArray();
    if (!sources.isEmpty())
        p.source = sources.first().toString();
    p.date = o.value(QStringLiteral("created_at")).toString();
    p.postUrl = baseUrl + QStringLiteral("/posts/") + QString::number(p.id);
    // Comment/note presence is stamped after finishRemote below: its
    // generic switch only knows top-level keys, e621 nests under stats/has.
}

Post fromE621(const QJsonObject &o, const QString &baseUrl) {
    Post p;
    e621FillCommon(p, o, baseUrl);
    // Flat tag blob (no per-category splits in either shape): everything
    // arrives general + untyped, exactly like a LOSN flat import. The
    // shared classifier waterfall resolves artist/character/copyright
    // (Danbooru-compatible 0/1/3/4 numbering) when enabled; species/lore
    // stay general until the e621-aware resolution lands (see below).
    QStringList tags;
    for (const QJsonValue &tv : o.value(QStringLiteral("tags")).toArray()) {
        if (tv.isString())
            tags << tv.toString();
    }
    const QStringList clean = cleanTagTokens(tags);
    p.tags = clean.join(u' ');
    p.tagsGeneral = p.tags;
    p.untyped = p.tags;
    QJsonObject norm(o);
    const QString rawMd5 =
        o.value(QStringLiteral("files")).toObject().value(QStringLiteral("meta")).toObject().value(
            QStringLiteral("md5")).toString();
    if (!rawMd5.isEmpty())
        norm[QStringLiteral("md5")] = rawMd5.toLower();
    // Variants affordance: viewerpage checks rawJson parent_id / children
    // the way it does for Danbooru.
    const QJsonObject rel = o.value(QStringLiteral("relationships")).toObject();
    const qint64 parentId = BooruEngineOps::jsonInt(rel.value(QStringLiteral("parent_id")));
    if (parentId != 0)
        norm[QStringLiteral("parent_id")] = parentId;
    if (!rel.value(QStringLiteral("children")).toArray().isEmpty())
        norm[QStringLiteral("has_active_children")] = true;
    BooruEngineOps::finishRemote(p, QStringLiteral("e621"), norm,
                                 BooruEngine::E621, QUrl(baseUrl).host());
    // finishRemote rebuilds tags from the (empty) buckets and its generic
    // comment switch cannot see e621's nesting: restore both.
    p.tags = p.tagsGeneral;
    const QJsonObject stats = o.value(QStringLiteral("stats")).toObject();
    p.commentCount = BooruEngineOps::jsonInt32(stats.value(QStringLiteral("comment_count")));
    p.hasComments = p.commentCount > 0;
    p.hasNotes = o.value(QStringLiteral("has")).toObject().value(QStringLiteral("notes")).toBool();
    return p;
}

} // namespace RemotePosts
