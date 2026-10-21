#pragma once

// Per-engine booru implementations. RemoteSource (remotesource.h) owns the
// network client; each engine family extends it through BooruEngineOps:
// search-URL building, response parsing, single-post refresh URLs and
// request auth. Host quirks (api subdomain, default cookies, PoW walls)
// come from sitequirks.h and are applied here, so engines stay correct
// per URL without the client knowing the one-offs.
//
// Pure helpers shared by the engines (taxonomy, API-object -> Post
// mapping, tag-syntax rewrites) stay free functions in RemotePosts.
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QNetworkRequest>
#include <QPair>
#include <QSet>
#include <QStringList>
#include <QUrl>

#include "boorutypes.h"
#include "sitequirks.h"

namespace RemotePosts {
// Engine taxonomy.
QString engineKey(BooruEngine engine); // "danbooru"|"gelbooru"|"moebooru"|...
BooruEngine engineForKey(const QString &key); // unknown -> Danbooru
QString engineLabel(BooruEngine engine);      // UI combo text
// Rewrite rating filters to the engine's native syntax ("safe" has no
// universal spelling: Gelbooru wants rating:general, Szurubooru wants
// safety:safe/unsafe). Single-letter shorthands (rating:g/s/q/e) expand
// to full words first. Anything else passes through untouched.
QStringList translateSearchTags(const QStringList &tags, BooruEngine engine);
// Native sort metatag for one UI sort mode ("added" and unknown modes
// have no server equivalent: empty). Only verified spellings are mapped
// (Danbooru order:*, Gelbooru sort:*:*, Moebooru order:id); anything else
// stays client-side via sortPosts. Explicit user sort:/order: tokens
// always win: callers skip this when the query already has one.
QString sortSearchTag(const QString &sort, BooruEngine engine);
// True when a failed search is worth one automatic retry (rate limits,
// timeouts, 5xx — never auth/walls/bad filters). A fresh search that
// lands nothing but these gets a delayed retry instead of a dead grid.
bool isTransientSearchError(int httpStatus, const QString &error);
// Stable slug for a new custom booru ("My Booru" -> "mybooru", deduped).
QString uniqueBooruKey(const QString &label, const QSet<QString> &taken);
// HyBooru search/detail shapes (custom Hydrus-network engine).
// Summaries carry no tags (lazy detail via refresh); namespaces map to
// buckets with natives preserved.
Post fromHybooruSummary(const QJsonObject &o, const QString &baseUrl);
Post fromHybooru(const QJsonObject &o, const QString &baseUrl);
// Nuuru (Soybooru) shapes: /api/booru/posts search/detail, native
// Flag/Media/Meta/Series/Subvariant/Trend/Variant/Uncategorized taxonomy
// preserved via customTags; plain fetches hit a PoW wall, so traffic
// rides the browser-backed stack (powWall quirk).
Post fromNuuruSummary(const QJsonObject &o, const QString &baseUrl);
Post fromNuuru(const QJsonObject &o, const QString &baseUrl);
// Entry default tags onto a query: space-split, appended in order,
// minus tab-dropped tokens (pill ×). Pure: unit-tested.
QStringList defaultTagsFor(const QStringList &base, const QString &defaults,
                           const QSet<QString> &dropped);
// Per-engine API-object -> Post mapping (pure, unit-tested).
// Keeps rawJson so the downloader can store the object verbatim.
Post fromDanbooru(const QJsonObject &o, const QString &baseUrl);
Post fromGelbooru(const QJsonObject &o, const QString &baseUrl);
Post fromMoebooru(const QJsonObject &o, const QString &baseUrl);
Post fromSzuru(const QJsonObject &o, const QString &baseUrl);
Post fromShimmie(const QMap<QString, QString> &attrs, const QString &baseUrl);
// Shimmie XML search response -> posts (same shape as the danbooru_api ext).
SearchResult parseShimmieXml(const BooruSite &site, const QByteArray &body);
QString postPageUrl(const QString &siteKey, const QString &baseUrl, qint64 id);
QString downloadFileName(const QString &siteLabel, const QJsonObject &raw);
QString shortFor(const QString &siteLabel); // DAN/GEL/SFW/YAN/KON/SZU
QString thumbUrlFor(const Post &p);  // smallest usable remote preview
QString viewerUrlFor(const Post &p); // sample preferred, full fallback
bool isImageUrl(const QString &url); // image ext (pool pipeline) vs video
// Reverse image search endpoints for a remote file URL: label + search
// URL pairs for menus (SauceNAO, Yandex, Google Lens, IQDB, trace.moe).
// Empty unless the URL is fetchable http(s): engines cannot take local
// files, so local posts get no actions. Pure: unit-tested.
QVector<QPair<QString, QString>>
reverseSearchUrls(const QString &fileUrl);
// Per-type disabled tag buckets ("gelbooru:character", engineKey form).
// Parsed tags in those buckets fold into general (non-anime taxonomies).
// Pushed from settings at startup / prefs apply; "general" never folds.
void setDisabledBuckets(const QSet<QString> &disabled);
bool bucketDisabled(BooruEngine engine, const QString &bucket);
// Per-host override ("gelbooru.com:character"): the form the prefs grid
// manages. engine-scoped keys keep working as a fallback (pre-rename
// settings); see the loadSettings migration.
bool bucketDisabledFor(const QString &host, const QString &bucket);
} // namespace RemotePosts

// One implementation per engine family (engine_danbooru.cpp and
// friends). Selected by engineOps(); RemoteSource calls through it.
class BooruEngineOps {
public:
    virtual ~BooruEngineOps() = default;
    struct Context {
        BooruEngine engine = BooruEngine::Danbooru;
        QString baseUrl;
        SiteCreds creds;
    };
    // Page-search request for native-syntax tags.
    virtual QUrl searchUrl(const Context &ctx, const QStringList &tags,
                           int page, int limit) const = 0;
    // Page-search response -> posts (sets result.site from siteKey).
    virtual SearchResult parseSearch(const QString &siteKey, const Context &ctx,
                                     const QByteArray &body) const = 0;
    // Single-post re-fetch URL for the refresh target id.
    virtual QUrl refreshUrl(const Context &ctx, qint64 id) const = 0;
    // Single-post body -> raw API object (ok=false when absent).
    virtual QJsonObject parseSingle(const QByteArray &body, bool &ok) const = 0;
    // Engine-native request auth (Basic / Token / none).
    virtual void applyAuth(QNetworkRequest &req,
                           const SiteCreds &creds) const = 0;
    // Quirk-aware API root (post pages stay on the base URL).
    QString apiBase(const Context &ctx) const;

    // Mapping helpers shared by the engines and the pure post helpers
    // below (public so the per-engine from* mappers can use them).
    static qint64 jsonInt(const QJsonValue &v, qint64 def = 0);
    // Range-checked int variant (scores, dimensions): hostile values
    // fall back instead of overflowing the cast.
    static int jsonInt32(const QJsonValue &v, int def = 0);
    static QString extOf(const QString &fileUrl,
                         const QString &fallback = QStringLiteral("jpg"));
    static QString joinTags(const QStringList &tags);
    static QStringList splitTags(const QString &s);
    // Safebooru-style dapi answers carry RELATIVE file URLs: resolve
    // against the host; absolute URLs pass through untouched.
    static QString resolveFileUrl(const QString &baseUrl, const QString &url);
    static void finishRemote(Post &p, const QString &siteLabel,
                             const QJsonObject &raw, BooruEngine engine,
                             const QString &host = {});
    // JSON response helpers: null document on syntax error; first object
    // of an array shape (ok=false when absent).
    static QJsonDocument parseJsonBody(const QByteArray &body);
    static QJsonObject firstOfArray(const QJsonArray &arr, bool &ok);
};

// Singleton ops for an engine family (never null; unknown -> Danbooru).
const BooruEngineOps *engineOps(BooruEngine engine);
