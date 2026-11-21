// Philomena-family engines: Philomena and BooruOnRails.
//
// Philomena (Elixir) is Derpibooru's current engine and serves the v1 JSON
// API:
//   search /api/v1/json/search/images?q=&page=&per_page=  -> {images, total}
//   single /api/v1/json/images/:id                        -> {image}
//   tags   /api/v1/json/search/tags?q=                    -> {tags}
//
// BooruOnRails (Ruby) is the codebase Philomena replaced; Twibooru still
// runs it and serves the same fields under v3 paths and envelopes:
//   search /api/v3/search/posts                           -> {posts}
//   single /api/v3/posts/:id                              -> {post}
//   tags   /api/v3/search/tags?q=                         -> {tags}
//
// Only the paths, the envelope keys and the preferred file URL differ, so
// one parser/engine is parameterised by PhilomenaShape instead of forking
// two near-identical files.
#include "booruengine.h"
#include "engineinternal.h"

#include <QJsonArray>
#include <QUrlQuery>

namespace RemotePosts {
namespace {

struct PhilomenaShape {
    QString apiRoot;    // API root below the site URL
    QString searchPath; // search endpoint from the site root
    QString itemPath;   // single-post endpoint up to the id
    QString listKey;    // search envelope array key
    QString itemKey;    // single envelope object key
    QString pagePath;   // human post page up to the id
    bool viewUrlFirst;  // v3 trusts view_url; v1 prefers representations
};

const PhilomenaShape &shapeFor(BooruEngine engine) {
    static const PhilomenaShape v1{
        QStringLiteral("/api/v1/json"),
        QStringLiteral("/api/v1/json/search/images"),
        QStringLiteral("/api/v1/json/images/"), QStringLiteral("images"),
        QStringLiteral("image"), QStringLiteral("/images/"), false};
    static const PhilomenaShape v3{
        QStringLiteral("/api/v3"), QStringLiteral("/api/v3/search/posts"),
        QStringLiteral("/api/v3/posts/"), QStringLiteral("posts"),
        QStringLiteral("post"), QStringLiteral("/posts/"), true};
    return engine == BooruEngine::BooruOnRails ? v3 : v1;
}

// Grid/search objects carry no site name. Keep the canonical pony hosts on
// their historic labels (favourites and download names are keyed on them)
// and use the host for every other instance instead of claiming Derpibooru.
QString siteNameFor(const QString &baseUrl) {
    const QString host = QUrl(baseUrl).host().toLower();
    if (host == QStringLiteral("derpibooru.org") ||
        host.endsWith(QStringLiteral(".derpibooru.org")))
        return QStringLiteral("Derpibooru");
    if (host == QStringLiteral("twibooru.org") ||
        host.endsWith(QStringLiteral(".twibooru.org")))
        return QStringLiteral("Twibooru");
    return host;
}

Post fromShape(BooruEngine engine, const QJsonObject &o, const QString &baseUrl) {
    const PhilomenaShape &s = shapeFor(engine);
    Post p;
    p.isRemote = true;
    p.siteName = siteNameFor(baseUrl);
    p.id = BooruEngineOps::jsonInt(o.value(QStringLiteral("id")));
    p.score = BooruEngineOps::jsonInt32(o.value(QStringLiteral("score")));
    p.width = qMax(0, BooruEngineOps::jsonInt32(o.value(QStringLiteral("width"))));
    p.height = qMax(0, BooruEngineOps::jsonInt32(o.value(QStringLiteral("height"))));
    p.source = o.value(QStringLiteral("source_url")).toString();
    if (p.source.isEmpty()) {
        const QJsonArray srcs = o.value(QStringLiteral("source_urls")).toArray();
        if (!srcs.isEmpty())
            p.source = srcs.first().toString();
    }
    p.date = o.value(QStringLiteral("created_at")).toString();

    const QJsonObject reps = o.value(QStringLiteral("representations")).toObject();
    auto rep = [&](const char *k) { return reps.value(QLatin1String(k)).toString(); };
    const QString viewUrl = o.value(QStringLiteral("view_url")).toString();
    if (s.viewUrlFirst) {
        p.fileUrl = viewUrl.isEmpty() ? rep("full") : viewUrl;
    } else {
        p.fileUrl = rep("full");
        if (p.fileUrl.isEmpty()) p.fileUrl = viewUrl;
    }
    p.sampleUrl = rep("large");
    if (p.sampleUrl.isEmpty()) p.sampleUrl = rep("medium");
    p.previewUrl = rep("thumb");
    if (p.previewUrl.isEmpty()) p.previewUrl = rep("small");
    p.ext = o.value(QStringLiteral("format")).toString().toLower();
    if (p.ext.isEmpty())
        p.ext = BooruEngineOps::extOf(p.fileUrl, QStringLiteral("png"));
    p.postUrl = baseUrl + s.pagePath + QString::number(p.id);

    // Tags: plain strings, some namespaced (artist:, character:, oc:,
    // spoiler:, content:). Rating is a tag: safe/suggestive/questionable/
    // explicit plus the grimdark pair, which also stay in meta.
    QStringList artists, chars, meta, general;
    const QJsonArray tags = o.value(QStringLiteral("tags")).toArray();
    for (const QJsonValue &tv : tags) {
        const QString t = tv.toString().trimmed();
        if (t.isEmpty()) continue;
        const QString tl = t.toLower();
        if (tl == QStringLiteral("safe")) { p.rating = QStringLiteral("safe"); continue; }
        if (tl == QStringLiteral("suggestive") || tl == QStringLiteral("questionable")) {
            p.rating = QStringLiteral("questionable"); continue;
        }
        if (tl == QStringLiteral("explicit")) { p.rating = QStringLiteral("explicit"); continue; }
        if (tl == QStringLiteral("semi-grimdark") || tl == QStringLiteral("grimdark")) {
            if (p.rating.isEmpty() || p.rating == QStringLiteral("safe"))
                p.rating = QStringLiteral("questionable");
            meta << t; continue;
        }
        if (t.startsWith(QStringLiteral("artist:"), Qt::CaseInsensitive)) artists << t;
        else if (t.startsWith(QStringLiteral("character:"), Qt::CaseInsensitive) ||
                 t.startsWith(QStringLiteral("oc:"), Qt::CaseInsensitive))
            chars << t;
        else if (t.startsWith(QStringLiteral("spoiler:"), Qt::CaseInsensitive) ||
                 t.startsWith(QStringLiteral("content:"), Qt::CaseInsensitive))
            meta << t;
        else general << t;
    }
    if (p.rating.isEmpty()) p.rating = QStringLiteral("general");
    p.tagsArtist = artists.join(u' ');
    p.tagsCharacter = chars.join(u' ');
    p.tagsMeta = meta.join(u' ');
    p.tagsGeneral = general.join(u' ');
    p.tags = (artists + chars + meta + general).join(u' ');
    BooruEngineOps::finishRemote(p, p.siteName, o, engine);
    return p;
}

class EnginePhilomena : public BooruEngineOps {
public:
    explicit EnginePhilomena(BooruEngine engine) : m_engine(engine) {}

    QUrl searchUrl(const Context &ctx, const QStringList &tags, int page,
                   int limit) const override {
        QUrl url(ctx.baseUrl + shapeFor(m_engine).searchPath);
        QUrlQuery q;
        // Philomena search: comma-separated terms; default to everything.
        const QString joined = tags.isEmpty() ? QStringLiteral("*")
                                              : tags.join(u',');
        q.addQueryItem(QStringLiteral("q"), joined);
        q.addQueryItem(QStringLiteral("page"), QString::number(qMax(1, page)));
        q.addQueryItem(QStringLiteral("per_page"), QString::number(qBound(1, limit, 50)));
        url.setQuery(q);
        return url;
    }
    SearchResult parseSearch(const QString &siteKey, const Context &ctx,
                             const QByteArray &body) const override {
        SearchResult r;
        r.site = siteKey;
        const QJsonDocument doc = parseJsonBody(body);
        if (doc.isNull() || !doc.isObject()) {
            r.error = QStringLiteral("Unparseable response");
            return r;
        }
        const QJsonArray arr =
            doc.object().value(shapeFor(m_engine).listKey).toArray();
        for (const QJsonValue &v : arr)
            if (v.isObject())
                r.posts.append(RemotePosts::fromPhilomenaFamily(
                    m_engine, v.toObject(), ctx.baseUrl));
        return r;
    }
    QUrl refreshUrl(const Context &ctx, qint64 id) const override {
        return QUrl(ctx.baseUrl + shapeFor(m_engine).itemPath +
                    QString::number(id));
    }
    QJsonObject parseSingle(const QByteArray &body, bool &ok) const override {
        ok = false;
        const QJsonDocument doc = parseJsonBody(body);
        if (!doc.isObject()) return {};
        const QJsonObject obj =
            doc.object().value(shapeFor(m_engine).itemKey).toObject();
        if (obj.isEmpty()) return {};
        ok = true;
        return obj;
    }
    void applyAuth(QNetworkRequest &, const SiteCreds &) const override {}

private:
    BooruEngine m_engine;
};
} // namespace

bool isPhilomenaFamily(BooruEngine engine) {
    return engine == BooruEngine::Philomena ||
           engine == BooruEngine::BooruOnRails;
}

bool needsTagHydration(BooruEngine engine) {
    return engine == BooruEngine::Hybooru ||
           engine == BooruEngine::AnimePictures;
}

QString philomenaApiRoot(BooruEngine engine) { return shapeFor(engine).apiRoot; }

QString philomenaListKey(BooruEngine engine) { return shapeFor(engine).listKey; }
QString philomenaItemKey(BooruEngine engine) { return shapeFor(engine).itemKey; }
QString philomenaItemPath(BooruEngine engine) { return shapeFor(engine).itemPath; }

Post fromPhilomenaFamily(BooruEngine engine, const QJsonObject &o,
                         const QString &baseUrl) {
    return fromShape(engine, o, baseUrl);
}
} // namespace RemotePosts

namespace EngineInternal {
const BooruEngineOps *philomenaOps() {
    static const RemotePosts::EnginePhilomena ops(BooruEngine::Philomena);
    return &ops;
}
const BooruEngineOps *booruOnRailsOps() {
    static const RemotePosts::EnginePhilomena ops(BooruEngine::BooruOnRails);
    return &ops;
}
} // namespace EngineInternal
