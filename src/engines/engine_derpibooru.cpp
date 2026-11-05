// Derpibooru engine (Philomena v1 JSON API).
// Search: /api/v1/json/search/images?q=&page=&per_page=  -> {images, total}
// Single: /api/v1/json/images/:id                        -> {image}
// Tags: /api/v1/json/search/tags?q=                      -> {tags}
#include "booruengine.h"

#include <QJsonArray>
#include <QUrlQuery>

namespace RemotePosts {
Post fromDerpibooru(const QJsonObject &o, const QString &baseUrl) {
    Post p;
    p.isRemote = true;
    p.siteName = QStringLiteral("Derpibooru");
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
    p.fileUrl = rep("full");
    if (p.fileUrl.isEmpty())
        p.fileUrl = o.value(QStringLiteral("view_url")).toString();
    p.sampleUrl = rep("large");
    if (p.sampleUrl.isEmpty()) p.sampleUrl = rep("medium");
    p.previewUrl = rep("thumb");
    if (p.previewUrl.isEmpty()) p.previewUrl = rep("small");
    p.ext = o.value(QStringLiteral("format")).toString().toLower();
    if (p.ext.isEmpty())
        p.ext = BooruEngineOps::extOf(p.fileUrl, QStringLiteral("png"));
    p.postUrl = baseUrl + QStringLiteral("/images/") + QString::number(p.id);

    // Tags: plain strings, some namespaced (artist:, character:, oc:, species:).
    // Rating is a tag: safe/suggestive/questionable/explicit/semi-grimdark/grimdark.
    QStringList artists, chars, copyrights, meta, general;
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
    p.tagsCopyright = copyrights.join(u' ');
    p.tagsMeta = meta.join(u' ');
    p.tagsGeneral = general.join(u' ');
    p.tags = (artists + chars + copyrights + meta + general).join(u' ');
    BooruEngineOps::finishRemote(p, QStringLiteral("Derpibooru"), o, BooruEngine::Derpibooru);
    return p;
}
} // namespace RemotePosts

namespace {
class EngineDerpibooru : public BooruEngineOps {
public:
    QUrl searchUrl(const Context &ctx, const QStringList &tags, int page,
                   int limit) const override {
        QUrl url(ctx.baseUrl + QStringLiteral("/api/v1/json/search/images"));
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
        const QJsonArray arr = doc.object().value(QStringLiteral("images")).toArray();
        for (const QJsonValue &v : arr)
            if (v.isObject())
                r.posts.append(RemotePosts::fromDerpibooru(v.toObject(), ctx.baseUrl));
        return r;
    }
    QUrl refreshUrl(const Context &ctx, qint64 id) const override {
        return QUrl(ctx.baseUrl + QStringLiteral("/api/v1/json/images/") +
                    QString::number(id));
    }
    QJsonObject parseSingle(const QByteArray &body, bool &ok) const override {
        ok = false;
        const QJsonDocument doc = parseJsonBody(body);
        if (!doc.isObject()) return {};
        const QJsonObject img = doc.object().value(QStringLiteral("image")).toObject();
        if (img.isEmpty()) return {};
        ok = true;
        return img;
    }
    void applyAuth(QNetworkRequest &, const SiteCreds &) const override {}
};
} // namespace

namespace EngineInternal {
const BooruEngineOps *derpibooruOps() {
    static const EngineDerpibooru ops;
    return &ops;
}
} // namespace EngineInternal
