#include "enginedetect.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>

namespace {

QJsonDocument parse(const QByteArray &body) {
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &err);
    return err.error == QJsonParseError::NoError ? doc : QJsonDocument();
}

QJsonObject firstObject(const QJsonDocument &doc) {
    if (doc.isObject())
        return doc.object();
    if (!doc.isArray() || doc.array().isEmpty() ||
        !doc.array().first().isObject())
        return {};
    return doc.array().first().toObject();
}

QJsonObject fetchObject(const EngineDetect::Fetch &fetch, const QString &url) {
    return firstObject(parse(fetch(url)));
}

bool hasArray(const QJsonObject &o, const QString &key) {
    return o.value(key).isArray();
}

} // namespace

namespace EngineDetect {

Result detect(const QString &base, const Fetch &fetch) {
    QString b = base.trimmed();
    while (b.endsWith(u'/'))
        b.chop(1);
    if (b.isEmpty())
        return {};
    const auto found = [](BooruEngine engine, const QString &baseUrl) {
        Result r;
        r.engine = engine;
        r.baseUrl = baseUrl;
        r.found = true;
        return r;
    };

    // e-shuushuu: FastAPI image list with total/images. Probe first: its
    // /search page is HTML, but the API path is unambiguous.
    {
        const QJsonObject o = fetchObject(
            fetch, b + QStringLiteral("/api/v1/images?per_page=1"));
        if (hasArray(o, QStringLiteral("images")) &&
            o.contains(QStringLiteral("total")))
            return found(BooruEngine::EShuushuu, b);
    }

    // Shimmie: XML only (danbooru_api extension).
    {
        const QByteArray xml = fetch(
            b + QStringLiteral("/api/danbooru/post/index.xml?limit=1"));
        if (xml.trimmed().startsWith('<') && xml.contains("<posts"))
            return found(BooruEngine::Shimmie, b);
    }

    // Szurubooru: version endpoint, else results-shaped listing. Some
    // instances live under a /booru subpath (no shared subdomain).
    QStringList cands = {b};
    if (!b.endsWith(QStringLiteral("/booru")))
        cands << b + QStringLiteral("/booru");
    for (const QString &cand : cands) {
        const QJsonDocument info =
            parse(fetch(cand + QStringLiteral("/api/info")));
        if (info.isObject() &&
            (info.object().contains(QStringLiteral("serverTime")) ||
             info.object().contains(QStringLiteral("version"))))
            return found(BooruEngine::Szurubooru, cand);
        const QJsonDocument list =
            parse(fetch(cand + QStringLiteral("/api/posts/?limit=1")));
        if (list.isObject() &&
            list.object().value(QStringLiteral("results")).isArray())
            return found(BooruEngine::Szurubooru, cand);
    }

    // e621: bare /posts.json array, files.original shape (Danbooru forks
    // with flat tags, but not Danbooru's tag_string_* fields).
    {
        const QJsonObject o =
            firstObject(parse(fetch(b + QStringLiteral("/posts.json?limit=1"))));
        if (!o.isEmpty() && o.value(QStringLiteral("files")).isObject() &&
            o.value(QStringLiteral("files"))
                .toObject()
                .contains(QStringLiteral("original")))
            return found(BooruEngine::E621, b);
    }

    // Danbooru: per-bucket tag_string_* fields.
    {
        const QJsonObject o =
            firstObject(parse(fetch(b + QStringLiteral("/posts.json?limit=1"))));
        if (!o.isEmpty() &&
            (o.contains(QStringLiteral("tag_string_artist")) ||
             o.contains(QStringLiteral("tag_string_general")) ||
             o.contains(QStringLiteral("tag_string_copyright"))))
            return found(BooruEngine::Danbooru, b);
    }

    // AnimePictures: v3 posts envelope with md5/ext summaries.
    {
        const QJsonObject o = fetchObject(
            fetch, b + QStringLiteral("/api/v3/posts?page=0"
                                      "&posts_per_page=1&lang=en"));
        if (hasArray(o, QStringLiteral("posts"))) {
            const QJsonObject p = o.value(QStringLiteral("posts"))
                                      .toArray()
                                      .first()
                                      .toObject();
            if (p.contains(QStringLiteral("md5")) &&
                p.contains(QStringLiteral("ext")))
                return found(BooruEngine::AnimePictures, b);
        }
    }

    // HyBooru: /api/post summaries (sha256/mime/extension, no tags).
    {
        const QJsonObject o = fetchObject(
            fetch, b + QStringLiteral("/api/post?page=0&pageSize=1"));
        if (hasArray(o, QStringLiteral("posts"))) {
            const QJsonObject p = o.value(QStringLiteral("posts"))
                                      .toArray()
                                      .first()
                                      .toObject();
            if (p.contains(QStringLiteral("sha256")) ||
                p.contains(QStringLiteral("mime")) ||
                p.contains(QStringLiteral("extension")))
                return found(BooruEngine::Hybooru, b);
        }
    }

    // Philomena v1: /api/v1/json/search/images -> {images:[...]}.
    {
        const QJsonObject o = fetchObject(
            fetch, b + QStringLiteral("/api/v1/json/search/images"
                                      "?q=%2A&page=1&per_page=1"));
        if (hasArray(o, QStringLiteral("images")) &&
            o.contains(QStringLiteral("total")))
            return found(BooruEngine::Philomena, b);
    }

    // BooruOnRails v3: /api/v3/search/posts -> {posts:[...]}.
    {
        const QJsonObject o = fetchObject(
            fetch, b + QStringLiteral("/api/v3/search/posts"
                                      "?q=%2A&page=1&per_page=1"));
        if (hasArray(o, QStringLiteral("posts")))
            return found(BooruEngine::BooruOnRails, b);
    }

    // Moebooru: flat "tags" string (epoch dates where present).
    // Gelbooru never serves /post.json, so anything tags-shaped here is
    // moebooru-family.
    {
        const QJsonObject o =
            firstObject(parse(fetch(b + QStringLiteral("/post.json?limit=1"))));
        if (!o.isEmpty() && o.value(QStringLiteral("tags")).isString())
            return found(BooruEngine::Moebooru, b);
    }

    // Nuuru / pow-walled hosts answer 449 pow_required / captcha_required.
    {
        const QByteArray body =
            fetch(b + QStringLiteral("/api/booru/posts?limit=1"));
        if (body.contains("pow_required") ||
            body.contains("captcha_required"))
            return found(BooruEngine::Nuuru, b);
    }

    // Gelbooru dapi family: {"post": [...]} or a bare tags array.
    {
        const QByteArray body = fetch(
            b + QStringLiteral("/index.php?page=dapi&s=post&q=index"
                               "&limit=1&json=1"));
        const QJsonDocument doc = parse(body);
        if (!doc.isNull()) {
            if (doc.isObject() &&
                doc.object().value(QStringLiteral("post")).isArray())
                return found(BooruEngine::Gelbooru, b);
            if (doc.isArray() && !doc.array().isEmpty()) {
                const QJsonValue v = doc.array().first();
                if (v.isObject() &&
                    v.toObject().value(QStringLiteral("tags")).isString())
                    return found(BooruEngine::Gelbooru, b);
            }
        }
    }
    return {};
}

} // namespace EngineDetect
