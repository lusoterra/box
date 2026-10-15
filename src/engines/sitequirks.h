#pragma once

// Per-host quirks: the one-offs that belong to a URL, not an engine.
// gelbooru.com expects a fringeBenefits cookie for restricted content,
// rule34.xxx serves its captcha wall on the main host (API calls go to
// the api subdomain), booru.allthefallen.moe guards its API with a
// proof-of-work challenge that re-appears roughly daily even for logins.
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
// Cleaned list -> "a=1; b=2" header value (empty when none).
QString cookieHeaderValue(const QStringList &cookies);
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
// Host matching for one cookie (exact, subdomain either way, or
// host-only session cookies).
bool cookieBelongsTo(const QNetworkCookie &cookie, const QString &host);
} // namespace RemotePosts
