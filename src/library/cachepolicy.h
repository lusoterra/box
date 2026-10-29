#pragma once

// On-disk media cache policy (thumbnails + full-quality browsing cache).
//
// Three global modes: cache everything, cache SFW only, cache nothing.
// Each configured booru can narrow that further (inherit / everything /
// SFW only / nothing): the effective mode is always the stricter of the
// global mode and the entry's own mode, so a per-booru choice can opt out
// but never opt back in past a global "nothing".
//
// "Nothing" only stops app-cache writes. Explicit downloads (the Download
// action) always write to the download folder, and viewed media still
// stages in the OS temp dir because the image/video readers need a file.
//
// SFW only has one exception: media the user explicitly downloaded (bytes
// present under the download folder, `Post::downloaded`) may be cached
// even when NSFW. Favouriting/bookmarking alone does NOT qualify - a
// favourite with no downloaded file stays subject to the rating filter.
// "Nothing" remains a hard floor: no disk cache, downloads included.
//
// Ratings: general/safe are SFW. sensitive/questionable/explicit and any
// unknown rating count as NSFW (fail closed when deciding what to purge).
#include <QHash>
#include <QList>
#include <QString>

#include "boorutypes.h"
#include "post.h"

namespace MediaCache {

inline CacheMode &globalModeRef() {
    static CacheMode mode = CacheMode::All;
    return mode;
}

inline QHash<QString, CacheMode> &siteModesRef() {
    static QHash<QString, CacheMode> modes;
    return modes;
}

inline QString modeKey(CacheMode mode) {
    switch (mode) {
    case CacheMode::Inherit:
        return QStringLiteral("inherit");
    case CacheMode::SfwOnly:
        return QStringLiteral("sfw");
    case CacheMode::None:
        return QStringLiteral("none");
    case CacheMode::All:
    default:
        return QStringLiteral("all");
    }
}

inline CacheMode modeFromKey(const QString &key, CacheMode fallback) {
    const QString k = key.trimmed().toLower();
    if (k == QStringLiteral("all"))
        return CacheMode::All;
    if (k == QStringLiteral("sfw") || k == QStringLiteral("sfw-only"))
        return CacheMode::SfwOnly;
    if (k == QStringLiteral("none") || k == QStringLiteral("nothing"))
        return CacheMode::None;
    if (k == QStringLiteral("inherit"))
        return CacheMode::Inherit;
    return fallback;
}

inline CacheMode stricter(CacheMode a, CacheMode b) {
    // Lower value = more permissive (All=0, SfwOnly=1, None=2). Inherit
    // never participates here (treated as the other side).
    if (a == CacheMode::Inherit)
        return b;
    if (b == CacheMode::Inherit)
        return a;
    return int(a) >= int(b) ? a : b;
}

inline void configure(CacheMode global, const QList<BooruSite> &boorus) {
    globalModeRef() = global == CacheMode::Inherit ? CacheMode::All : global;
    QHash<QString, CacheMode> &modes = siteModesRef();
    modes.clear();
    for (const BooruSite &b : boorus) {
        if (!b.key.isEmpty() && b.cacheMode != CacheMode::Inherit)
            modes.insert(b.key, b.cacheMode);
    }
}

inline CacheMode globalMode() {
    return globalModeRef();
}

inline CacheMode effectiveMode(const QString &siteKey) {
    CacheMode mode = globalMode();
    if (!siteKey.isEmpty()) {
        const auto it = siteModesRef().constFind(siteKey);
        if (it != siteModesRef().constEnd())
            mode = stricter(mode, it.value());
    }
    return mode;
}

inline bool isSfwRating(const QString &rating) {
    const QString r = rating.trimmed().toLower();
    return r.isEmpty() || r == QStringLiteral("general") ||
           r == QStringLiteral("safe");
}

inline bool isNsfwRating(const QString &rating) {
    return !isSfwRating(rating);
}

// Disk persistence for one media item from a given site. `siteKey` may be
// empty (local archive rows): those follow the global mode only.
// `explicitDownload` marks bytes the user asked to keep (download folder):
// they bypass the SFW rating gate, never the "Nothing" floor.
inline bool persistFor(const QString &siteKey, const QString &rating,
                       bool explicitDownload = false) {
    const CacheMode mode = effectiveMode(siteKey);
    if (mode == CacheMode::None)
        return false;
    if (mode == CacheMode::SfwOnly && isNsfwRating(rating) &&
        !explicitDownload)
        return false;
    return true;
}

inline bool persistForPost(const Post &post) {
    // Per-booru choices describe booru traffic; local archive thumbnails
    // follow the global mode (the files are already on disk anyway).
    return persistFor(post.isRemote ? post.siteKey : QString(), post.rating,
                      post.downloaded);
}

} // namespace MediaCache
