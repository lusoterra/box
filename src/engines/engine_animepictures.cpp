// AnimePictures engine (anime-pictures.net, SFW anime; ~670k posts).
// Shapes verified live 2026-11-21 (public, no account) + Grabber's
// model.ts + Boorusama's anime_pictures client:
//   GET https://api.anime-pictures.net/api/v3/posts?page={0-based}
//       &search_tag=...&denied_tags=...&posts_per_page=N&lang=en
//     -> {posts: [...], posts_count, max_pages (index), ...}
//   GET https://api.anime-pictures.net/api/v3/posts/<id>
//     -> {post: {...}, source: {kind, url, ...}, tags: [{tag: {tag,
//        num, type, ...}, ...}], ...}
// Summaries carry NO tags (HyBooru-style hydration through refreshUrl).
// Media URLs are md5-derived (Boorusama createUrl/Grabber
// completeImage): previews //opreviews.<host>/<md5[:3]>/<md5>_?p.avif,
// full //oimages.<host>/<md5[:3]>/<md5>.<ext>.
// Tag types (verified): 1 character, 4 author->artist, 3/5/6
// copyright*, 2 reference->general, 7 object->general, 0 unknown->general.
// Site is SFW: erotics level (0 none, 1 light, 2 moderate, 3 hard) folds
// to general/sensitive/questionable (never explicit); unknown -> general.
// (* copyrightProduct/copyrightGame/copyrightOther collapse together;
// parent/alias chains are not followed.)
#include "booruengine.h"
#include "engineinternal.h"
#include "explicittags.h"

#include <QUrlQuery>

namespace {

// Media URLs are md5-derived (Boorusama createUrl / Grabber completeImage):
// previews //opreviews.<host>/<md5[:3]>/<md5>_{sp,cp,bp}.avif, full
// //oimages.<host>/<md5[:3]>/<md5>.<ext>.
QString animeMediaUrl(const QString &apiBase, const QString &md5,
                      const QString &suffix, const QString &ext,
                      bool preview) {
    if (md5.size() < 3)
        return {};
    QString root = apiBase;
    root.replace(QStringLiteral("https://api."),
                 preview ? QStringLiteral("https://opreviews.")
                         : QStringLiteral("https://oimages."));
    if (!root.endsWith(u'/'))
        root += u'/';
    return root + md5.left(3) + u'/' + md5 + suffix + ext;
}

QString animeRating(const QJsonObject &post) {
    // erotics: 0 none, 1 light, 2 moderate, 3 hard (Boorusama EroticLevel).
    const int level = post.value(QStringLiteral("erotics")).toInt(-1);
    if (level <= 0)
        return QStringLiteral("general");
    if (level == 1)
        return QStringLiteral("sensitive");
    if (level >= 2)
        return QStringLiteral("questionable");
    return QStringLiteral("general");
}

void animePlaceTag(const QString &name, int type, QString &artist,
                   QString &character, QString &copyright, QString &general) {
    // Box tags are space-separated: normalize the site's spaced names
    // ("long hair" -> "long_hair", Danbooru style). searchUrl() converts
    // back when talking to the API.
    QString bare = name.trimmed().simplified().replace(u' ', u'_');
    if (!tagTokenOk(bare))
        return;
    QString *field = &general;
    if (type == 1)
        field = &character;
    else if (type == 4)
        field = &artist;
    else if (type == 3 || type == 5 || type == 6)
        field = &copyright;
    if (!field->isEmpty())
        *field += u' ';
    *field += bare;
}

// The ext field carries a leading dot (".jpg"); previews are .avif.
QString animeExt(const QJsonObject &post) {
    QString ext = post.value(QStringLiteral("ext")).toString().toLower();
    while (ext.startsWith(u'.'))
        ext = ext.mid(1);
    return ext;
}

class EngineAnimePictures : public BooruEngineOps {
public:
    QUrl searchUrl(const Context &ctx, const QStringList &tags, int page,
                   int limit) const override {
        QUrl url(RemotePosts::apiBaseUrl(ctx.engine, ctx.baseUrl) +
                 QStringLiteral("/api/v3/posts"));
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("page"),
                           QString::number(qMax(0, page - 1)));
        query.addQueryItem(QStringLiteral("posts_per_page"),
                           QString::number(qBound(1, limit, 100)));
        QStringList include, denied;
        // Box tokens use underscores; the API wants spaces.
        auto toApi = [](QString t) {
            t.replace(u'_', u' ');
            return t;
        };
        for (const QString &t : tags) {
            if (t.startsWith(u'-') && t.size() > 1)
                denied << toApi(t.mid(1));
            else if (!t.isEmpty())
                include << toApi(t);
        }
        if (!include.isEmpty())
            query.addQueryItem(QStringLiteral("search_tag"), include.join(u' '));
        if (!denied.isEmpty())
            query.addQueryItem(QStringLiteral("denied_tags"), denied.join(u' '));
        query.addQueryItem(QStringLiteral("lang"), QStringLiteral("en"));
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
            if (!v.isObject())
                continue;
            Post p = RemotePosts::fromAnimePicturesSummary(v.toObject(),
                                                           ctx.baseUrl);
            if (p.id == 0 || p.fileUrl.isEmpty())
                continue; // unusable rows (no md5 to reconstruct from)
            p.siteKey = siteKey; // refresh targets resolve exactly
            result.posts.append(std::move(p));
        }
        return result;
    }
    QUrl refreshUrl(const Context &ctx, qint64 id) const override {
        return QUrl(RemotePosts::apiBaseUrl(ctx.engine, ctx.baseUrl) +
                    QStringLiteral("/api/v3/posts/") + QString::number(id));
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
        // Anonymous (no login-walled content on the public API).
    }
};

} // namespace

namespace EngineInternal {
const BooruEngineOps *animePicturesOps() {
    static const EngineAnimePictures ops;
    return &ops;
}
} // namespace EngineInternal

namespace RemotePosts {

void animeFillCommon(Post &p, const QJsonObject &post,
                     const QString &baseUrl) {
    p.id = BooruEngineOps::jsonInt(post.value(QStringLiteral("id")));
    const QString md5 = post.value(QStringLiteral("md5")).toString().toLower();
    const QString ext = animeExt(post);
    p.ext = ext;
    const QString apiBase =
        RemotePosts::apiBaseUrl(BooruEngine::AnimePictures, baseUrl);
    // Previews: authoritative when present, reconstructed otherwise
    // (Boorusama createUrl: _sp/_cp/_bp .avif).
    p.previewUrl = post.value(QStringLiteral("small_preview")).toString();
    if (p.previewUrl.isEmpty())
        p.previewUrl = animeMediaUrl(apiBase, md5, QStringLiteral("_sp"),
                                     QStringLiteral(".avif"), true);
    p.sampleUrl = post.value(QStringLiteral("big_preview")).toString();
    if (p.sampleUrl.isEmpty())
        p.sampleUrl = animeMediaUrl(apiBase, md5, QStringLiteral("_bp"),
                                    QStringLiteral(".avif"), true);
    // Full file reconstructed the Grabber/Boorusama way (oimages host).
    p.fileUrl = animeMediaUrl(apiBase, md5, QString(), u'.' + ext, false);
    p.width = BooruEngineOps::jsonInt32(post.value(QStringLiteral("width")));
    p.height = BooruEngineOps::jsonInt32(post.value(QStringLiteral("height")));
    p.rating = animeRating(post);
    p.score = BooruEngineOps::jsonInt32(post.value(QStringLiteral("score_number")));
    p.date = post.value(QStringLiteral("pubtime")).toString();
    p.postUrl = baseUrl + QStringLiteral("/pictures/view_post/") +
                QString::number(p.id);
}

Post fromAnimePicturesSummary(const QJsonObject &o, const QString &baseUrl) {
    // Tagless summary (like HyBooru): tags hydrate through refreshUrl.
    Post p;
    animeFillCommon(p, o, baseUrl);
    QJsonObject norm(o);
    const QString md5 = o.value(QStringLiteral("md5")).toString().toLower();
    if (!md5.isEmpty())
        norm[QStringLiteral("md5")] = md5;
    BooruEngineOps::finishRemote(p, QStringLiteral("AnimePictures"), norm,
                                 BooruEngine::AnimePictures,
                                 QUrl(baseUrl).host());
    return p;
}

Post fromAnimePictures(const QJsonObject &detail, const QString &baseUrl) {
    // Detail envelope: {post: {...}, source: {kind, url, ...},
    // tags: [{tag: {tag, num, type, ...}, ...}], ...}.
    const QJsonObject post = detail.value(QStringLiteral("post")).toObject();
    Post p;
    animeFillCommon(p, post.isEmpty() ? detail : post, baseUrl);
    const QJsonObject source = detail.value(QStringLiteral("source")).toObject();
    const QString sourceUrl = source.value(QStringLiteral("url")).toString();
    if (isHttpUrl(sourceUrl))
        p.source = sourceUrl;
    QString artist, character, copyright, general;
    for (const QJsonValue &tv : detail.value(QStringLiteral("tags")).toArray()) {
        if (!tv.isObject())
            continue;
        const QJsonObject t = tv.toObject().value(QStringLiteral("tag")).toObject();
        animePlaceTag(t.value(QStringLiteral("tag")).toString(),
                      t.value(QStringLiteral("type")).toInt(0), artist,
                      character, copyright, general);
    }
    p.tagsArtist = artist;
    p.tagsCharacter = character;
    p.tagsCopyright = copyright;
    p.tagsGeneral = general;
    // Untyped feed for the classifier: flat-import rule (rowToPost offers
    // the general blob when no bucket splits exist, but splits ARE known
    // here, so untyped stays empty and nothing refetches).
    QJsonObject norm(detail);
    const QString rawMd5 = post.value(QStringLiteral("md5")).toString();
    if (!rawMd5.isEmpty())
        norm[QStringLiteral("md5")] = rawMd5.toLower();
    BooruEngineOps::finishRemote(p, QStringLiteral("AnimePictures"), norm,
                                 BooruEngine::AnimePictures,
                                 QUrl(baseUrl).host());
    return p;
}

} // namespace RemotePosts
