#pragma once

// Core booru data types: engine taxonomy, credentials, site entries and
// search results. Pure data, no networking: shared by the RemoteSource
// client (remotesource.h), the per-engine implementations (booruengine.h)
// and the per-host quirks (sitequirks.h).
#include <QList>
#include <QMap>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

#include "post.h"

struct SiteCreds {
    QString user;   // danbooru login / gelbooru user ID / moebooru username
    QString secret; // danbooru API key / gelbooru API key / moebooru password
    bool complete() const { return !user.trimmed().isEmpty() && !secret.isEmpty(); }
};

struct SearchResult {
    QString site; // site key, e.g. "danbooru"
    QVector<Post> posts;
    QString error; // empty on success
    int httpStatus = 0; // HTTP code of the page fetch (0 = n/a)
    bool ok() const { return error.isEmpty(); }
};

// Engine taxonomy (mirrors LoliSnatcher's BooruType for these families).
enum class BooruEngine { Danbooru, Gelbooru, Moebooru, Safebooru, Szurubooru, Shimmie, Hybooru, Nuuru, Hiddenbooru };

struct BooruSite {
    QString key;      // stable id: "danbooru", "gelbooru", "yandere", ...
    QString label;    // UI name: "Danbooru" (renames orphan old favourites)
    QString shortName; // grid badge text, user-editable ("DAN", "R34", …)
    QString badgeColor = QStringLiteral("#3a7ec2"); // grid badge background
    QString baseUrl;  // empty = self-hosted, not configured yet
    BooruEngine engine;
    bool needsCreds = false; // login-walled listing (none currently enforce)
    QString user;     // per-booru credentials (empty = anonymous)
    QString secret;   // danbooru key / gelbooru key / moebooru password / token
    // Cookie bypass for captcha/Cloudflare-walled instances: normalized
    // "name=value" lines, sent as one Cookie header with every request
    // to the site (API + media). Empty = none.
    QStringList cookies;
    // Meme/other non-anime hosts: leave their tags alone (no fetching,
    // no applying verdicts). Defaults off.
    bool skipAutoClassify = false;
    // Buckets this host's taxonomy lacks (artist/character/copyright/
    // meta): folded into general at parse + classify time. Empty = all
    // on. Per-host replacement for the engine-family grid in Preferences.
    QSet<QString> disabledBuckets;
    // Extra tags for every search on this booru, added to the global
    // defaults (space-separated, same syntax as the omnibar).
    QString defaultTags;
    // Proxy for this booru's traffic (http://host:port or
    // socks5://[user:pass@]host:port). Empty = direct connection.
    QString proxy;
};
