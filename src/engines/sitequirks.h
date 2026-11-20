#pragma once

// Per-host quirks: the one-offs that belong to a URL, not an engine.
// gelbooru.com expects a fringeBenefits cookie for restricted content,
// rule34.xxx serves its captcha wall on the main host (API calls go to
// the api subdomain), booru.allthefallen.moe guards its API with a
// proof-of-work challenge that re-appears roughly daily even for logins,
// soybooru.com stacks the same JS wall on Cloudflare (plain fetches get
// HTTP 449 / pow_required).
// Pure functions over the host: no registry, no network.
#include <QList>
#include <QNetworkCookie>
#include <QString>
#include <QStringList>
#include <QUrl>

#include "boorutypes.h"

namespace RemotePosts {
struct SiteQuirk {
    QString host;               // exact lowercase host, no scheme
    QString apiHost;            // API base override, empty = same host
    QStringList defaultCookies; // seeded once into new matching entries
    bool powWall = false;       // JS proof-of-work, re-solves periodically
    // Cloudflare-fronted API/CDN: `cf_clearance` is bound to the browser
    // fingerprint, so QNAM answers 403 even with a freshly harvested
    // cookie. API searches, refreshes, tag suggestions and media ride
    // the capture profile instead of burning a doomed plain request.
    bool browserApi = false;
};
QList<SiteQuirk> siteQuirks();
const SiteQuirk *quirkForHost(const QString &host);
// Same lookup from a full base URL ("https://rule34.xxx" -> its quirk).
const SiteQuirk *quirkForUrl(const QString &baseUrl);
// API root for requests (post pages stay on the base URL): quirk hosts
// may override it (rule34.xxx -> https://api.rule34.xxx).
QString apiBaseUrl(BooruEngine engine, const QString &baseUrl);
// Cookie lines ("name=value", one per line; a pasted "a=1; b=2" header
// or Set-Cookie attributes are tolerated) -> cleaned list, later wins,
// garbage dropped.
QStringList normalizeCookies(const QStringList &lines);
// Cleaned list -> "a=1; b=2" header value (empty when none). Legacy
// unscoped form for tests/simple callers; requests must use
// cookieHeaderValueForHost so scoped entries land on the right host.
QString cookieHeaderValue(const QStringList &cookies);
// Request-specific header (empty when none). Entries may be unscoped
// ("cf_clearance=x", belongs to the entry base host) or host-scoped
// ("cdn.donmai.us!cf_clearance=x"). Unscoped entries follow the base host
// (exact/subdomain/known media CDN); scoped entries only apply under
// their own host. Scoped values win when a name collides.
QString cookieHeaderValueForHost(const QStringList &cookies,
                                 const QString &requestHost,
                                 const QString &siteHost);
// Known same-operator media CDNs that are siblings (not subdomains) of
// a booru host: Danbooru/Safebooru -> cdn.donmai.us, Derpibooru ->
// derpicdn.net. Used by cookie routing and per-entry proxies.
bool isKnownMediaHost(const QString &siteHost, const QString &requestHost);
// Scope harvested cookies for the entry whose base host is `siteHost`.
// Cookies valid for the site itself (host-only/eq-site clearances) stay
// unscoped so known same-operator CDNs inherit them (one browser captcha
// unlocks both); CDN-specific captures are scoped to `host`. Domain
// cookies keep their domain. Pure.
QStringList harvestScopedCookies(const QList<QNetworkCookie> &seen,
                                 const QString &host,
                                 const QString &siteHost = {});
// Fetchable URL schemes: only http(s) ever leaves the machine. Anything
// else (file:///, javascript:, data:, custom schemes from a rogue API
// object) is refused before any request, read, or download.
bool isHttpUrl(const QString &url);
// Quirk default cookies missing from `current` (compared by cookie name,
// case-insensitive). Empty when the host has no quirk or nothing is
// missing: callers append the result to the stored list.
QStringList seedQuirkCookies(const QStringList &current, const QString &host);
// Normalized harvest of a raw cookie jar for one host (exact host or
// subdomain match, later values win, garbage dropped). Pure.
QStringList harvestCookies(const QList<QNetworkCookie> &seen,
                           const QString &host);
// Default grid-badge background for an entry: well-known hosts get their
// house colors (gelbooru.com blue, rule34.xxx family green, aibooru.online
// green, danbooru/safebooru tan, any shimmie beige-gray), everything else
// the neutral blue. Pure: user-picked colors are never touched by this,
// callers stamp it onto fresh entries only.
QString defaultBadgeColor(const QString &baseUrl, BooruEngine engine);
// Host matching for one cookie (exact, subdomain either way, or
// host-only session cookies).
bool cookieBelongsTo(const QNetworkCookie &cookie, const QString &host);
} // namespace RemotePosts
