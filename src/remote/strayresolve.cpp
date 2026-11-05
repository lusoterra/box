#include "strayresolve.h"

#include "explicittags.h"
#include "remotesource.h"

#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSet>
#include <QUrl>
#include <QUrlQuery>

namespace {

QString htmlDecode(QString s) {
    s.replace(QStringLiteral("&amp;"), QStringLiteral("&"));
    s.replace(QStringLiteral("&#38;"), QStringLiteral("&"));
    s.replace(QStringLiteral("&quot;"), QStringLiteral("\""));
    s.replace(QStringLiteral("&#39;"), QStringLiteral("'"));
    s.replace(QStringLiteral("&apos;"), QStringLiteral("'"));
    s.replace(QStringLiteral("&lt;"), QStringLiteral("<"));
    s.replace(QStringLiteral("&gt;"), QStringLiteral(">"));
    return s;
}

// One attrib="value" pair inside a tag/body. Empty when absent.
QString attr(const QString &text, const QString &name) {
    const QRegularExpression re(
        QStringLiteral("\\b%1\\s*=\\s*[\"']([^\"']*)[\"']").arg(name));
    const auto m = re.match(text);
    return m.hasMatch() ? htmlDecode(m.captured(1)) : QString();
}

// First <meta> whose property/name is one of `keys` -> its content.
QString metaContent(const QString &html, const QStringList &keys) {
    static const QRegularExpression meta(
        QStringLiteral("<meta\\b[^>]*>"),
        QRegularExpression::CaseInsensitiveOption);
    auto it = meta.globalMatch(html);
    while (it.hasNext()) {
        const QString tag = it.next().captured(0);
        QString key = attr(tag, QStringLiteral("property"));
        if (key.isEmpty())
            key = attr(tag, QStringLiteral("name"));
        key = key.trimmed().toLower();
        if (keys.contains(key)) {
            const QString content = attr(tag, QStringLiteral("content"));
            if (!content.isEmpty())
                return content;
        }
    }
    return {};
}

QStringList splitFlat(const QString &s) {
    return s.split(QRegularExpression(QStringLiteral("\\s+")),
                   Qt::SkipEmptyParts);
}

// Dedupe preserving order (bucket lists can repeat across HTML/API shapes).
QStringList uniqueTags(const QStringList &in) {
    QStringList out;
    QSet<QString> seen;
    for (const QString &t : in) {
        if (!seen.contains(t)) {
            seen.insert(t);
            out << t;
        }
    }
    return out;
}

QString originOf(const QUrl &u, const QString &prefix) {
    QString base = u.scheme().toLower() + QStringLiteral("://") +
                   u.host().toLower();
    const int port = u.port();
    const bool nonDefault =
        port > 0 &&
        !((u.scheme().toLower() == QStringLiteral("http") && port == 80) ||
          (u.scheme().toLower() == QStringLiteral("https") && port == 443));
    if (nonDefault)
        base += u':' + QString::number(port);
    QString p = prefix;
    while (p.endsWith(u'/'))
        p.chop(1);
    return base + p;
}

QString jsonPathString(const QJsonObject &o, const QString &a,
                       const QString &b = {}) {
    QJsonValue v = o.value(a);
    if (v.isString())
        return v.toString();
    if (!b.isEmpty() && v.isObject())
        return v.toObject().value(b).toString();
    return {};
}

// e621/other public shapes: tags live in an object keyed by category.
void genericTagsFromObject(const QJsonObject &tags, Post &p) {
    QStringList artist, character, copyright, general, meta;
    for (auto it = tags.begin(); it != tags.end(); ++it) {
        QStringList names;
        for (const QJsonValue &v : it.value().toArray()) {
            if (v.isString() && !v.toString().isEmpty())
                names << v.toString();
        }
        if (names.isEmpty())
            continue;
        const QString cat = it.key().toLower();
        if (cat == QStringLiteral("artist"))
            artist << names;
        else if (cat == QStringLiteral("character"))
            character << names;
        else if (cat == QStringLiteral("copyright") ||
                 cat == QStringLiteral("series") || cat == QStringLiteral("studio"))
            copyright << names;
        else if (cat == QStringLiteral("meta") ||
                 cat == QStringLiteral("metadata"))
            meta << names;
        else // species, lore, general, contributor, invalid, ...
            general << names;
    }
    auto assign = [&](QString &dst, const QStringList &v) {
        if (dst.trimmed().isEmpty() && !v.isEmpty())
            dst = cleanTagTokens(v).join(u' ');
    };
    assign(p.tagsArtist, artist);
    assign(p.tagsCharacter, character);
    assign(p.tagsCopyright, copyright);
    assign(p.tagsGeneral, general);
    assign(p.tagsMeta, meta);
}

void genericFillPost(Post &p, const QJsonObject &raw, const QString &baseUrl) {
    if (!RemotePosts::isHttpUrl(p.fileUrl))
        p.fileUrl = jsonPathString(raw, QStringLiteral("file_url"));
    if (!RemotePosts::isHttpUrl(p.fileUrl))
        p.fileUrl = jsonPathString(raw, QStringLiteral("file"),
                                   QStringLiteral("url"));
    if (!RemotePosts::isHttpUrl(p.fileUrl))
        p.fileUrl = jsonPathString(raw, QStringLiteral("large_file_url"));
    if (!RemotePosts::isHttpUrl(p.fileUrl))
        p.fileUrl = jsonPathString(raw, QStringLiteral("url"));
    if (p.fileUrl.isEmpty())
        return;
    // Relative media URLs (rare outside dapi) resolve against the page.
    if (!RemotePosts::isHttpUrl(p.fileUrl) && !baseUrl.isEmpty())
        p.fileUrl = baseUrl + (p.fileUrl.startsWith(u'/') ? p.fileUrl
                                                          : u'/' + p.fileUrl);
    if (p.sampleUrl.isEmpty())
        p.sampleUrl = jsonPathString(raw, QStringLiteral("sample_url"));
    if (p.sampleUrl.isEmpty())
        p.sampleUrl = jsonPathString(raw, QStringLiteral("sample"),
                                     QStringLiteral("url"));
    if (p.previewUrl.isEmpty())
        p.previewUrl = jsonPathString(raw, QStringLiteral("preview_url"));
    if (p.previewUrl.isEmpty())
        p.previewUrl = jsonPathString(raw, QStringLiteral("preview"),
                                      QStringLiteral("url"));
    if (p.ext.isEmpty())
        p.ext = BooruEngineOps::extOf(p.fileUrl,
                                      jsonPathString(raw, QStringLiteral("file"),
                                                     QStringLiteral("ext")));
    if (p.md5.isEmpty()) {
        const QString md5 = jsonPathString(raw, QStringLiteral("md5"));
        if (!md5.isEmpty())
            p.md5 = md5;
    }
    if (p.md5.isEmpty())
        p.md5 = jsonPathString(raw, QStringLiteral("checksumMD5"));
    if (p.md5.isEmpty())
        p.md5 = jsonPathString(raw, QStringLiteral("file"), QStringLiteral("md5"));
    auto num = [](const QJsonValue &v) -> int {
        if (v.isDouble())
            return static_cast<int>(v.toDouble());
        if (v.isString())
            return v.toString().toInt();
        return 0;
    };
    if (p.width <= 0)
        p.width = num(raw.value(QStringLiteral("width")));
    if (p.width <= 0 && raw.value(QStringLiteral("file")).isObject())
        p.width = num(raw.value(QStringLiteral("file"))
                          .toObject()
                          .value(QStringLiteral("width")));
    if (p.height <= 0)
        p.height = num(raw.value(QStringLiteral("height")));
    if (p.height <= 0 && raw.value(QStringLiteral("file")).isObject())
        p.height = num(raw.value(QStringLiteral("file"))
                           .toObject()
                           .value(QStringLiteral("height")));
    if (p.rating.isEmpty() || p.rating == QStringLiteral("general"))
        if (raw.value(QStringLiteral("rating")).isString())
            p.rating = normalizeRating(
                raw.value(QStringLiteral("rating")).toString());
    if (p.score == 0) {
        const QJsonValue score = raw.value(QStringLiteral("score"));
        p.score = score.isObject()
                      ? num(score.toObject().value(QStringLiteral("total")))
                      : num(score);
    }
    if (p.source.isEmpty()) {
        const QJsonValue src = raw.value(QStringLiteral("source"));
        if (src.isString())
            p.source = src.toString();
        else if (src.isArray() && !src.toArray().isEmpty())
            p.source = src.toArray().first().toString();
        else if (raw.value(QStringLiteral("sources")).isArray() &&
                 !raw.value(QStringLiteral("sources")).toArray().isEmpty())
            p.source = raw.value(QStringLiteral("sources"))
                           .toArray()
                           .first()
                           .toString();
    }
    if (p.date.isEmpty())
        p.date = raw.value(QStringLiteral("created_at")).toString();
    if (p.date.isEmpty())
        p.date = raw.value(QStringLiteral("posted")).toString();
    if (p.title.isEmpty())
        p.title = raw.value(QStringLiteral("title")).toString();
    if (p.tags.trimmed().isEmpty()) {
        const QJsonValue tags = raw.value(QStringLiteral("tags"));
        if (tags.isString()) {
            p.tagsGeneral = tags.toString();
        } else if (tags.isObject()) {
            genericTagsFromObject(tags.toObject(), p);
        } else if (tags.isArray()) {
            QStringList general;
            for (const QJsonValue &v : tags.toArray()) {
                if (!v.isObject())
                    continue;
                const QJsonObject o = v.toObject();
                const QString cat = o.value(QStringLiteral("category"))
                                        .toString()
                                        .toLower();
                const QStringList names = cleanTagTokens(
                    [&] {
                        QStringList n;
                        for (const QJsonValue &t : o.value(QStringLiteral("names"))
                                                       .toArray())
                            n << t.toString();
                        return n;
                    }());
                if (names.isEmpty())
                    continue;
                if (cat == QStringLiteral("artist"))
                    p.tagsArtist = cleanTagTokens(splitFlat(p.tagsArtist) + names)
                                       .join(u' ');
                else if (cat == QStringLiteral("character"))
                    p.tagsCharacter =
                        cleanTagTokens(splitFlat(p.tagsCharacter) + names)
                            .join(u' ');
                else if (cat == QStringLiteral("copyright"))
                    p.tagsCopyright =
                        cleanTagTokens(splitFlat(p.tagsCopyright) + names)
                            .join(u' ');
                else if (cat == QStringLiteral("meta") ||
                         cat == QStringLiteral("metadata"))
                    p.tagsMeta = cleanTagTokens(splitFlat(p.tagsMeta) + names)
                                     .join(u' ');
                else
                    general << names;
            }
            if (p.tagsGeneral.trimmed().isEmpty() && !general.isEmpty())
                p.tagsGeneral = cleanTagTokens(general).join(u' ');
        }
    }
    QStringList all;
    all << splitFlat(p.tagsArtist) << splitFlat(p.tagsCharacter)
        << splitFlat(p.tagsCopyright) << splitFlat(p.tagsGeneral)
        << splitFlat(p.tagsMeta);
    p.tags = uniqueTags(cleanTagTokens(all)).join(u' ');
}

} // namespace

namespace RemotePosts {

BooruPostUrl parseBooruPostUrl(const QString &url) {
    BooruPostUrl out;
    if (!isHttpUrl(url))
        return out;
    const QUrl u(url);
    if (!u.isValid() || u.host().isEmpty())
        return out;
    const QString path = u.path();
    const QUrlQuery query(u);

    auto withPrefix = [&](const QString &prefix) {
        out.baseUrl = originOf(u, prefix);
    };
    auto idOk = [&](const QString &s) {
        bool ok = false;
        const qint64 id = s.toLongLong(&ok);
        return ok && id > 0 ? id : 0;
    };

    // Query-shaped post pages (Gelbooru-family and Shimmie).
    if (query.queryItemValue(QStringLiteral("page")) ==
            QStringLiteral("post") &&
        query.queryItemValue(QStringLiteral("s")) == QStringLiteral("view")) {
        const qint64 id = idOk(query.queryItemValue(QStringLiteral("id")));
        if (id > 0) {
            withPrefix(path.section(u'/', 0, -2));
            out.id = id;
            out.engines = {BooruEngine::Gelbooru};
            return out;
        }
    }
    {
        const QString q = query.queryItemValue(QStringLiteral("q"));
        static const QRegularExpression shim(
            QStringLiteral("^post/view/(\\d+)"));
        const auto m = shim.match(q.trimmed());
        if (m.hasMatch()) {
            withPrefix(path.section(u'/', 0, -2));
            out.id = m.captured(1).toLongLong();
            out.engines = {BooruEngine::Shimmie};
            return out;
        }
    }

    // Path-shaped post pages.
    auto pathMatch = [&](const QRegularExpression &re) {
        const auto m = re.match(path);
        if (!m.hasMatch())
            return false;
        withPrefix(m.captured(1));
        out.id = m.captured(2).toLongLong();
        return out.id > 0;
    };

    static const QRegularExpression view(
        QStringLiteral("^(.*)/post/view/(\\d+)(?:/.*)?$"));
    if (pathMatch(view)) {
        // Nuuru post pages and Shimmie post pages share this shape; probe
        // both APIs (the first is a cheap 404 on the other family).
        out.engines = {BooruEngine::Nuuru, BooruEngine::Shimmie};
        return out;
    }
    static const QRegularExpression show(
        QStringLiteral("^(.*)/post/show/(\\d+)(?:/.*)?$"));
    if (pathMatch(show)) {
        out.engines = {BooruEngine::Moebooru};
        return out;
    }
    static const QRegularExpression derpi(
        QStringLiteral("^(.*)/images/(\\d+)(?:/.*)?$"));
    if (pathMatch(derpi)) {
        out.engines = {BooruEngine::Derpibooru};
        return out;
    }
    static const QRegularExpression posts(
        QStringLiteral("^(.*)/posts/(\\d+)(?:/.*)?$"));
    if (pathMatch(posts)) {
        // Danbooru-family, HyBooru and Twibooru (Philomena v3) use /posts/<id>.
        out.engines = {BooruEngine::Danbooru, BooruEngine::Hybooru, BooruEngine::Twibooru};
        return out;
    }
    static const QRegularExpression single(
        QStringLiteral("^(.*)/post/(\\d+)(?:/.*)?$"));
    if (pathMatch(single)) {
        out.engines = {BooruEngine::Szurubooru};
        return out;
    }
    out = BooruPostUrl();
    return out;
}

Post parseBooruPageHtml(const QByteArray &body, const QString &pageUrl,
                        qint64 fallbackId) {
    Post p;
    const QString html = QString::fromUtf8(body);

    const QString media = metaContent(
        html, {QStringLiteral("og:image"), QStringLiteral("og:image:url"),
               QStringLiteral("og:image:secure_url"),
               QStringLiteral("twitter:image"),
               QStringLiteral("twitter:image:src")});
    if (media.isEmpty())
        return p;
    QString fileUrl = media;
    if (fileUrl.startsWith(QStringLiteral("//")))
        fileUrl = QUrl(pageUrl).scheme() + u':' + fileUrl;
    else if (fileUrl.startsWith(u'/'))
        fileUrl = QUrl(pageUrl).scheme() + QStringLiteral("://") +
                  QUrl(pageUrl).host() + fileUrl;
    if (!isHttpUrl(fileUrl))
        return p;
    p.fileUrl = fileUrl;
    p.sampleUrl = fileUrl;

    bool gotId = false;
    p.id = attr(html, QStringLiteral("data-id")).toLongLong(&gotId);
    if (!gotId || p.id <= 0)
        p.id = fallbackId;
    p.postUrl = pageUrl;
    p.md5 = attr(html, QStringLiteral("data-md5"));
    p.rating = normalizeRating(
        attr(html, QStringLiteral("data-rating")).isEmpty()
            ? metaContent(html, {QStringLiteral("rating")})
            : attr(html, QStringLiteral("data-rating")));
    p.score = attr(html, QStringLiteral("data-score")).toInt();
    p.width = attr(html, QStringLiteral("data-width")).toInt();
    p.height = attr(html, QStringLiteral("data-height")).toInt();
    p.source = attr(html, QStringLiteral("data-source"));
    p.title = metaContent(html, {QStringLiteral("og:title")});
    if (p.title.isEmpty())
        p.title = metaContent(html, {QStringLiteral("twitter:title")});
    if (p.ext.isEmpty())
        p.ext = BooruEngineOps::extOf(p.fileUrl);

    // Bucketed tag list (Gelbooru-family pages): <li class="tag-type-X">
    // ... <a href="...&amp;tags=<tagname>">. Falls back to the flat
    // data-tags attribute when the page has no typed list.
    QStringList artist, character, copyright, general, meta;
    static const QRegularExpression li(
        QStringLiteral("<li\\b[^>]*\\bclass\\s*=\\s*[\"'][^\"']*\\b"
                       "tag-type-([A-Za-z_]+)\\b[^\"']*[\"'][^>]*>(.*?)</li>"),
        QRegularExpression::DotMatchesEverythingOption |
            QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression hrefTag(
        QStringLiteral("(?:[?&]|&amp;)(?:tags|search)=([^\"'&<>\\s]+)"));
    auto it = li.globalMatch(html);
    while (it.hasNext()) {
        const auto m = it.next();
        const QString kind = m.captured(1).toLower();
        const auto h = hrefTag.match(m.captured(2));
        if (!h.hasMatch())
            continue;
        const QString tag = htmlDecode(QUrl::fromPercentEncoding(
                                           h.captured(1).toUtf8()))
                                .trimmed();
        if (tag.isEmpty())
            continue;
        if (kind == QStringLiteral("artist"))
            artist << tag;
        else if (kind == QStringLiteral("character"))
            character << tag;
        else if (kind == QStringLiteral("copyright") ||
                 kind == QStringLiteral("series") ||
                 kind == QStringLiteral("studio"))
            copyright << tag;
        else if (kind == QStringLiteral("metadata") ||
                 kind == QStringLiteral("meta"))
            meta << tag;
        else
            general << tag;
    }
    if (artist.isEmpty() && character.isEmpty() && copyright.isEmpty() &&
        general.isEmpty() && meta.isEmpty()) {
        general = splitFlat(attr(html, QStringLiteral("data-tags")));
    }
    // Guard against generic social previews (site banners on 404/challenge
    // pages): only a page that actually carries booru post markup counts.
    const bool booruMarkup = !artist.isEmpty() || !character.isEmpty() ||
                             !copyright.isEmpty() || !general.isEmpty() ||
                             !meta.isEmpty() || !p.md5.isEmpty();
    if (!booruMarkup)
        return Post();
    p.tagsArtist = uniqueTags(cleanTagTokens(artist)).join(u' ');
    p.tagsCharacter = uniqueTags(cleanTagTokens(character)).join(u' ');
    p.tagsCopyright = uniqueTags(cleanTagTokens(copyright)).join(u' ');
    p.tagsGeneral = uniqueTags(cleanTagTokens(general)).join(u' ');
    p.tagsMeta = uniqueTags(cleanTagTokens(meta)).join(u' ');
    QStringList all;
    all << splitFlat(p.tagsArtist) << splitFlat(p.tagsCharacter)
        << splitFlat(p.tagsCopyright) << splitFlat(p.tagsGeneral)
        << splitFlat(p.tagsMeta);
    p.tags = uniqueTags(all).join(u' ');
    p.untyped = p.tagsGeneral;
    p.hasMeta = true;
    return p;
}

Post postFromRaw(BooruEngine engine, const QJsonObject &raw,
                 const QString &baseUrl) {
    Post p;
    switch (engine) {
    case BooruEngine::Danbooru:
        p = fromDanbooru(raw, baseUrl);
        break;
    case BooruEngine::Gelbooru:
    case BooruEngine::Safebooru:
        p = fromGelbooru(raw, baseUrl);
        break;
    case BooruEngine::Moebooru:
        p = fromMoebooru(raw, baseUrl);
        break;
    case BooruEngine::Szurubooru:
        p = fromSzuru(raw, baseUrl);
        break;
    case BooruEngine::Shimmie: {
        QMap<QString, QString> attrs;
        for (auto it = raw.begin(); it != raw.end(); ++it)
            attrs.insert(it.key(),
                         it.value().isString()
                             ? it.value().toString()
                             : QString::number(it.value().toDouble()));
        p = fromShimmie(attrs, baseUrl);
        break;
    }
    case BooruEngine::Hybooru:
        p = fromHybooru(raw, baseUrl);
        break;
    case BooruEngine::Nuuru:
        p = fromNuuru(raw, baseUrl);
        break;
    case BooruEngine::Derpibooru:
        p = fromDerpibooru(raw, baseUrl);
        break;
    case BooruEngine::Twibooru:
        p = fromTwibooru(raw, baseUrl);
        break;
    }
    genericFillPost(p, raw, baseUrl);
    return p;
}

} // namespace RemotePosts

StrayResolver::StrayResolver(QObject *parent)
    : QObject(parent), m_nam(new QNetworkAccessManager(this)) {}

QNetworkAccessManager *StrayResolver::namForProxy(const QString &proxySpecIn) {
    if (proxySpecIn.trimmed().isEmpty())
        return m_nam;
    auto it = m_proxyNams.find(proxySpecIn);
    if (it != m_proxyNams.end())
        return it.value();
    const QNetworkProxy proxy = RemotePosts::proxyFromSpec(proxySpecIn);
    if (proxy.type() == QNetworkProxy::NoProxy)
        return m_nam;
    QNetworkAccessManager *nam = new QNetworkAccessManager(this);
    nam->setProxy(proxy);
    m_proxyNams.insert(proxySpecIn, nam);
    return nam;
}

QString StrayResolver::proxySpec() const {
    QString spec = RemotePosts::proxyForUrl(m_match.baseUrl);
    if (spec.trimmed().isEmpty())
        spec = RemotePosts::globalProxySpec();
    return spec;
}

void StrayResolver::cancel() {
    m_cancelled = true;
    if (m_reply) {
        disconnect(m_reply, nullptr, this, nullptr);
        m_reply->abort();
        m_reply->deleteLater();
        m_reply = nullptr;
    }
    m_urls.clear();
    m_results.clear();
    m_index = 0;
}

void StrayResolver::resolve(const QStringList &urls) {
    cancel();
    m_cancelled = false;
    for (const QString &raw : urls) {
        const QString t = raw.trimmed();
        if (!t.isEmpty())
            m_urls << t;
    }
    if (m_urls.isEmpty()) {
        emit finished(m_results);
        return;
    }
    startNext();
}

void StrayResolver::startNext() {
    if (m_cancelled)
        return;
    if (m_index >= m_urls.size()) {
        emit finished(m_results);
        return;
    }
    const QString url = m_urls.at(m_index);
    emit progress(m_index, m_urls.size(), url);
    m_htmlTried = false;
    m_candidate = 0;
    m_match = RemotePosts::parseBooruPostUrl(url);
    if (!RemotePosts::isHttpUrl(url)) {
        failCurrent(tr("Only http(s) links can be used"));
        return;
    }
    if (!m_match.valid()) {
        // Everything else is passed to the downloader as-is: the content
        // sniff there rejects HTML/wall pages with a clear error.
        Post p;
        p.isRemote = true;
        p.fileUrl = url;
        p.ext = BooruEngineOps::extOf(url, QString());
        acceptPost(p, false);
        return;
    }
    requestApi();
}

void StrayResolver::requestApi() {
    if (m_cancelled)
        return;
    if (m_candidate >= m_match.engines.size()) {
        tryHtmlFallback();
        return;
    }
    const BooruEngine engine = m_match.engines.at(m_candidate++);
    const BooruEngineOps *ops = engineOps(engine);
    const QString host = QUrl(m_match.baseUrl).host().toLower();
    const QString key = RemotePosts::hostKeys().value(host);
    const BooruSite *site = key.isEmpty() ? nullptr : RemoteSource::siteFor(key);
    const SiteCreds creds = site ? SiteCreds{site->user, site->secret}
                                 : SiteCreds{};
    const BooruEngineOps::Context ctx{engine, m_match.baseUrl, creds};
    const QUrl api = ops->refreshUrl(ctx, m_match.id);
    if (api.isEmpty()) {
        requestApi();
        return;
    }
    // Fingerprint/PoW-walled hosts: plain API requests get wall pages, so
    // ride the browser stack the same way refreshes do (when available).
    if (const RemotePosts::SiteQuirk *q = RemotePosts::quirkForHost(host);
        q && q->powWall) {
        if (const RemoteSource::WebFetchFn web = RemoteSource::webFetcher()) {
            const RemoteSource::WebFetchResult fetched = web(api, 30000);
            bool ok = false;
            const QJsonObject raw =
                fetched.body.isEmpty()
                    ? QJsonObject()
                    : ops->parseSingle(fetched.body, ok);
            if (ok && !raw.isEmpty() && fetched.error.isEmpty()) {
                acceptRaw(raw);
                return;
            }
            requestApi();
            return;
        }
    }
    QNetworkRequest req(api);
    req.setTransferTimeout(20000);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setHeader(QNetworkRequest::UserAgentHeader,
                  QStringLiteral("box/0.1 (stray resolve)"));
    RemoteSource::applyCookies(req);
    ops->applyAuth(req, creds);
    m_pendingEngine = engine;
    QNetworkReply *reply = namForProxy(proxySpec())->get(req);
    m_reply = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply, ops] {
        if (m_reply == reply)
            m_reply = nullptr;
        reply->deleteLater();
        if (m_cancelled)
            return;
        if (reply->error() != QNetworkReply::NoError) {
            requestApi(); // next family / HTML fallback
            return;
        }
        const QByteArray body = reply->readAll();
        if (body.isEmpty() || body.size() > 32 * 1024 * 1024) {
            requestApi();
            return;
        }
        bool ok = false;
        const QJsonObject raw = ops->parseSingle(body, ok);
        if (!ok || raw.isEmpty()) {
            requestApi();
            return;
        }
        acceptRaw(raw);
    });
}

void StrayResolver::acceptRaw(const QJsonObject &raw) {
    Post p = RemotePosts::postFromRaw(m_pendingEngine, raw, m_match.baseUrl);
    if (!RemotePosts::isHttpUrl(p.fileUrl)) {
        requestApi(); // wrong family guess: try the next API shape
        return;
    }
    if (p.postUrl.isEmpty())
        p.postUrl = m_urls.value(m_index);
    acceptPost(p, true);
}

void StrayResolver::tryHtmlFallback() {
    if (m_cancelled)
        return;
    if (m_htmlTried) {
        failCurrent(tr("Could not read this booru post (API and page both "
                       "refused the request)"));
        return;
    }
    m_htmlTried = true;
    const QString url = m_urls.at(m_index);
    QNetworkRequest req((QUrl(url)));
    req.setTransferTimeout(20000);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setHeader(QNetworkRequest::UserAgentHeader,
                  QStringLiteral("box/0.1 (stray resolve)"));
    req.setRawHeader("Accept", "text/html,application/xhtml+xml");
    RemoteSource::applyCookies(req);
    QNetworkReply *reply = namForProxy(proxySpec())->get(req);
    m_reply = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply, url] {
        if (m_reply == reply)
            m_reply = nullptr;
        reply->deleteLater();
        if (m_cancelled)
            return;
        if (reply->error() != QNetworkReply::NoError) {
            failCurrent(reply->errorString());
            return;
        }
        const QByteArray body = reply->readAll();
        const QString ct = reply->header(QNetworkRequest::ContentTypeHeader)
                               .toString()
                               .toLower();
        if (ct.startsWith(QStringLiteral("image/")) ||
            ct.startsWith(QStringLiteral("video/")) ||
            ct.startsWith(QStringLiteral("application/octet-stream"))) {
            Post p;
            p.isRemote = true;
            p.fileUrl = url;
            p.ext = BooruEngineOps::extOf(url, QString());
            acceptPost(p, false); // page URL served media directly
            return;
        }
        if (body.isEmpty() || body.size() > 32 * 1024 * 1024) {
            failCurrent(tr("Empty or oversized page"));
            return;
        }
        Post p = RemotePosts::parseBooruPageHtml(body, url, m_match.id);
        if (!RemotePosts::isHttpUrl(p.fileUrl)) {
            failCurrent(tr("This page exposes no downloadable media (login "
                           "or challenge wall?)"));
            return;
        }
        acceptPost(p, true);
    });
}

void StrayResolver::acceptPost(Post p, bool booru) {
    // Stray identity: URL-hash based (id 0), exactly like hand-added
    // strays, so two boorus' same-numbered posts never collide.
    p.isRemote = true;
    p.siteName = QStringLiteral("_Stray");
    p.siteKey.clear();
    p.siteShort.clear();
    p.id = 0;
    if (p.ext.isEmpty() && !p.fileUrl.isEmpty())
        p.ext = BooruEngineOps::extOf(p.fileUrl, QString());

    QJsonObject raw = p.rawJson;
    raw[QStringLiteral("stray")] = true;
    raw[QStringLiteral("tags")] = p.tags;
    if (!p.tagsArtist.isEmpty())
        raw[QStringLiteral("tags_artist")] = p.tagsArtist;
    if (!p.tagsCharacter.isEmpty())
        raw[QStringLiteral("tags_character")] = p.tagsCharacter;
    if (!p.tagsCopyright.isEmpty())
        raw[QStringLiteral("tags_copyright")] = p.tagsCopyright;
    if (!p.tagsGeneral.isEmpty())
        raw[QStringLiteral("tags_general")] = p.tagsGeneral;
    if (!p.tagsMeta.isEmpty())
        raw[QStringLiteral("tags_meta")] = p.tagsMeta;
    if (!p.postUrl.isEmpty())
        raw[QStringLiteral("post_url")] = p.postUrl;
    if (!p.fileUrl.isEmpty() &&
        raw.value(QStringLiteral("file_url")).toString().isEmpty())
        raw[QStringLiteral("file_url")] = p.fileUrl;
    if (!p.ext.isEmpty() &&
        raw.value(QStringLiteral("file_ext")).toString().isEmpty())
        raw[QStringLiteral("file_ext")] = p.ext;
    if (!p.md5.isEmpty() &&
        raw.value(QStringLiteral("md5")).toString().isEmpty())
        raw[QStringLiteral("md5")] = p.md5;
    p.rawJson = raw;

    Result r;
    r.sourceUrl = m_urls.value(m_index);
    r.post = p;
    r.booru = booru;
    r.ok = true;
    m_results.append(r);
    doneCurrent();
}

void StrayResolver::failCurrent(const QString &error) {
    Result r;
    r.sourceUrl = m_urls.value(m_index);
    r.ok = false;
    r.error = error;
    m_results.append(r);
    doneCurrent();
}

void StrayResolver::doneCurrent() {
    ++m_index;
    startNext();
}
