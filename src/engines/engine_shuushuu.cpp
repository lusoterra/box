// e-shuushuu (e-shuushuu.net): a custom anime image board with a
// FastAPI backend. Verified live 2026-11-26:
//   GET /api/v1/images?tags=<id,id>&exclude_tags=<id>&tags_mode=all
//       &status=1&page=N&per_page=M
//     -> {total,page,per_page,images:[...],comments:{...}}
//   GET /api/v1/images/<id>/ -> one image object
//   GET /api/v1/tags?search=<name>&per_page=20 -> {tags:[{tag_id,title,
//       type,alias_of,...}]}
// Images carry full/medium/thumb URLs, dimensions, md5, source_url,
// favorites and a tags array (type 1 Theme, 2 Source, 3 Artist,
// 4 Character). The site is SFW, so every post maps to general rating.
// The image search takes tag IDs, not names: RemoteSource resolves each
// query name through the tag endpoint first (resolvesTagIds()).
#include "booruengine.h"
#include "engineinternal.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QUrlQuery>

#include <utility>

namespace {

QString shuushuuTag(const QString &raw) {
    QString tag = raw.trimmed().simplified();
    tag.replace(u' ', u'_');
    return tagTokenOk(tag) ? tag : QString();
}

void shuushuuPlace(const QJsonObject &tag, QString &artist, QString &character,
                   QString &copyright, QString &general) {
    const QString title = shuushuuTag(tag.value(QStringLiteral("title")).toString());
    if (title.isEmpty())
        return;
    QString *field = &general;
    switch (BooruEngineOps::jsonInt32(tag.value(QStringLiteral("type")), 1)) {
    case 3: field = &artist; break;
    case 2: field = &copyright; break;
    case 4: field = &character; break;
    default: field = &general; break;
    }
    if (!field->isEmpty())
        *field += u' ';
    *field += title;
}

class EngineShuushuu : public BooruEngineOps {
public:
    QUrl searchUrl(const Context &ctx, const QStringList &tags, int page,
                   int limit) const override {
        // Not used directly: resolvesTagIds() routes through the tag
        // lookup first. Keep a page-shaped URL for diagnostics.
        Q_UNUSED(tags);
        QUrl url(ctx.baseUrl + QStringLiteral("/api/v1/images"));
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("page"),
                           QString::number(qMax(1, page)));
        query.addQueryItem(QStringLiteral("per_page"),
                           QString::number(qBound(1, limit, 100)));
        query.addQueryItem(QStringLiteral("status"), QStringLiteral("1"));
        url.setQuery(query);
        return url;
    }
    bool resolvesTagIds() const override { return true; }
    QUrl tagLookupUrl(const Context &ctx, const QString &name) const override {
        QUrl url(ctx.baseUrl + QStringLiteral("/api/v1/tags"));
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("search"), name);
        query.addQueryItem(QStringLiteral("per_page"), QStringLiteral("20"));
        query.addQueryItem(QStringLiteral("exclude_aliases"),
                           QStringLiteral("false"));
        url.setQuery(query);
        return url;
    }
    qint64 parseTagId(const QByteArray &body, const QString &name) const override {
        const QJsonDocument doc = parseJsonBody(body);
        if (doc.isNull() || !doc.isObject())
            return 0;
        const QString wanted = name.trimmed().toLower();
        for (const QJsonValue &v :
             doc.object().value(QStringLiteral("tags")).toArray()) {
            if (!v.isObject())
                continue;
            const QJsonObject tag = v.toObject();
            if (tag.value(QStringLiteral("title")).toString().trimmed().toLower() !=
                wanted)
                continue;
            const qint64 alias = jsonInt(tag.value(QStringLiteral("alias_of")));
            if (alias > 0)
                return alias;
            return jsonInt(tag.value(QStringLiteral("tag_id")));
        }
        return 0;
    }
    QUrl resolvedSearchUrl(const Context &ctx, const QVector<qint64> &include,
                           const QVector<qint64> &exclude, int page,
                           int limit) const override {
        QUrl url(ctx.baseUrl + QStringLiteral("/api/v1/images"));
        QUrlQuery query;
        auto ids = [](const QVector<qint64> &v) {
            QStringList out;
            out.reserve(v.size());
            for (qint64 id : v)
                if (id > 0)
                    out << QString::number(id);
            return out.join(u',');
        };
        const QString inc = ids(include);
        const QString exc = ids(exclude);
        if (!inc.isEmpty())
            query.addQueryItem(QStringLiteral("tags"), inc);
        if (!exc.isEmpty())
            query.addQueryItem(QStringLiteral("exclude_tags"), exc);
        query.addQueryItem(QStringLiteral("tags_mode"), QStringLiteral("all"));
        query.addQueryItem(QStringLiteral("status"), QStringLiteral("1"));
        query.addQueryItem(QStringLiteral("page"),
                           QString::number(qMax(1, page)));
        query.addQueryItem(QStringLiteral("per_page"),
                           QString::number(qBound(1, limit, 100)));
        query.addQueryItem(QStringLiteral("sort_by"),
                           QStringLiteral("image_id"));
        query.addQueryItem(QStringLiteral("sort_order"),
                           QStringLiteral("DESC"));
        url.setQuery(query);
        return url;
    }
    SearchResult parseSearch(const QString &siteKey, const Context &ctx,
                             const QByteArray &body) const override {
        SearchResult result;
        result.site = siteKey;
        const QJsonDocument doc = parseJsonBody(body);
        if (doc.isNull() || !doc.isObject() ||
            !doc.object().value(QStringLiteral("images")).isArray()) {
            result.error = doc.isNull()
                               ? QStringLiteral("Unparseable response")
                               : QStringLiteral("Unexpected response shape");
            return result;
        }
        for (const QJsonValue &v :
             doc.object().value(QStringLiteral("images")).toArray()) {
            if (!v.isObject())
                continue;
            Post p = RemotePosts::fromEshuushuu(v.toObject(), ctx.baseUrl);
            if (p.id == 0 || p.fileUrl.isEmpty())
                continue;
            p.siteKey = siteKey;
            result.posts.append(std::move(p));
        }
        return result;
    }
    QUrl refreshUrl(const Context &ctx, qint64 id) const override {
        return QUrl(ctx.baseUrl + QStringLiteral("/api/v1/images/") +
                    QString::number(id) + u'/');
    }
    QJsonObject parseSingle(const QByteArray &body, bool &ok) const override {
        ok = false;
        const QJsonDocument doc = parseJsonBody(body);
        if (doc.isNull() || !doc.isObject())
            return {};
        ok = doc.object().contains(QStringLiteral("image_id"));
        return ok ? doc.object() : QJsonObject();
    }
    void applyAuth(QNetworkRequest &req, const SiteCreds &creds) const override {
        Q_UNUSED(req);
        Q_UNUSED(creds); // public read-only API
    }
};

} // namespace

namespace EngineInternal {
const BooruEngineOps *shuushuuOps() {
    static const EngineShuushuu ops;
    return &ops;
}
} // namespace EngineInternal

namespace RemotePosts {

Post fromEshuushuu(const QJsonObject &o, const QString &baseUrl) {
    Post p;
    p.id = BooruEngineOps::jsonInt(o.value(QStringLiteral("image_id")));
    if (p.id <= 0)
        return p;
    QString ext = o.value(QStringLiteral("ext")).toString().toLower();
    while (ext.startsWith(u'.'))
        ext = ext.mid(1);
    if (ext.isEmpty())
        ext = BooruEngineOps::extOf(o.value(QStringLiteral("url")).toString());
    p.ext = ext;
    p.fileUrl = o.value(QStringLiteral("url")).toString();
    p.sampleUrl = o.value(QStringLiteral("large_url")).toString();
    if (p.sampleUrl.isEmpty())
        p.sampleUrl = o.value(QStringLiteral("medium_url")).toString();
    p.previewUrl = o.value(QStringLiteral("thumbnail_url")).toString();
    if (p.previewUrl.isEmpty())
        p.previewUrl = o.value(QStringLiteral("medium_url")).toString();
    p.width = BooruEngineOps::jsonInt32(o.value(QStringLiteral("width")));
    p.height = BooruEngineOps::jsonInt32(o.value(QStringLiteral("height")));
    p.score = BooruEngineOps::jsonInt32(o.value(QStringLiteral("favorites")));
    p.date = o.value(QStringLiteral("date_added")).toString();
    p.source = o.value(QStringLiteral("source_url")).toString();
    p.md5 = o.value(QStringLiteral("md5_hash")).toString().toLower();
    p.postUrl = baseUrl + QStringLiteral("/images/") + QString::number(p.id);
    p.rating = QStringLiteral("general"); // SFW board
    QString artist, character, copyright, general;
    for (const QJsonValue &tv : o.value(QStringLiteral("tags")).toArray()) {
        if (tv.isObject())
            shuushuuPlace(tv.toObject(), artist, character, copyright,
                          general);
    }
    p.tagsArtist = artist;
    p.tagsCharacter = character;
    p.tagsCopyright = copyright;
    p.tagsGeneral = general;
    BooruEngineOps::finishRemote(p, QStringLiteral("e-shuushuu"), o,
                                 BooruEngine::EShuushuu,
                                 QUrl(baseUrl).host());
    return p;
}

} // namespace RemotePosts
