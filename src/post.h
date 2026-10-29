#pragma once

#include <QString>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QSet>
#include <QStringList>

#include <ranges>

// Post record. Mirrors the fields the Python prototypes exposed
// (local archives, remote API results, favourites rows).
// over /api/posts so filtering/sorting behaves the same.
struct Post {
    QString file;   // relative path with '/' separators (local) or "" (remote)
    QString absFile; // absolute path (local only, for loading)
    QString ext;    // lowercase, no dot
    qint64 id = 0;
    int score = 0;
    QString rating = QStringLiteral("general"); // general|sensitive|questionable|explicit
    bool ratingInferred = false; // true when upgraded via EXPLICIT_TAGS
    QString tags;         // all tags, space separated
    QString tagsArtist;
    QString tagsCharacter;
    QString tagsCopyright;
    QString tagsMeta;
    QString tagsGeneral;
    // Native categories outside the five buckets (Szurubooru instances
    // define their own: meme, series extras, ...). Category -> tags in
    // arrival order. Searchable via `tags`, shown as their own groups in
    // the viewer; never folded, never classified away.
    QMap<QString, QStringList> customTags;
    // Tags that arrived without a category (LOSN tagType "none", Gelbooru
    // native blob). Initially bucketed as general; TagTypes classification
    // may move them later. Feeds Index::collectUntyped().
    QString untyped;
    int width = 0;
    int height = 0;
    QString source;
    QString date;
    QString title;
    QString postUrl; // direct link to the post page (LOSN postURL); may be empty
    QString category;
    bool hasMeta = false;
    // Comment/annotation presence for grid pills. Count when the API
    // gives one (Szuru commentCount, Gelbooru comment_count); otherwise a
    // last-comment/last-note stamp or flag means "has" (Danbooru/Moebooru
    // timestamps, Gelbooru has_notes). False = none or not exposed.
    int commentCount = 0;
    bool hasComments = false;
    bool hasNotes = false;
    // ── remote (browse mode) ──
    bool isRemote = false;
    QString siteName;   // "Danbooru", "Gelbooru", ...
    // Configured entry key behind a browse result ("rule34", "gelbooru",
    // custom slugs). Stamped at merge time; never persisted. Empty for
    // favourites/local posts, which resolve via siteName instead.
    QString siteKey;
    QString siteShort;  // grid badge: "DAN", "GEL", ...
    QString fileUrl;    // full-res media URL (may be empty when login-walled)
    QString sampleUrl;  // scaled preview URL (viewer prefers this)
    QString previewUrl; // tiny thumbnail URL (grid prefers this)
    // Content hash (lowercase hex) when the API provides one: lets the
    // same file collapse across entries (Tor onion vs I2P mirror twins).
    // Empty = unknown, never deduped on.
    QString md5;
    QJsonObject rawJson; // original API post object (written as download sidecar)
    // Runtime-only cache-policy hint (never persisted / never sent to the
    // sidecar): true only when the bytes are explicitly kept under the
    // download folder. MainWindow stamps it from isPostDownloaded() before
    // snapshots reach the grid, viewer and thumbnail queues. A favourite
    // without a downloaded file stays false, so "liked" alone never lifts
    // the SFW-only cache restriction.
    bool downloaded = false;
};

inline bool isVideoExt(const QString &ext) {
    return ext == QStringLiteral("mp4") || ext == QStringLiteral("webm");
}

// Media-kind extension sets for the omnibar media filter. Animated
// pictures (ugoira, animated webp/jxl) count as GIFs — never the reverse
// (gif conversion is lossy). Mirrors the grid's own video/GIF chips.
inline const QStringList &gifLikeExts() {
    static const QStringList exts = {QStringLiteral("gif"),
                                     QStringLiteral("webp"),
                                     QStringLiteral("jxl")};
    return exts;
}
inline const QStringList &videoExts() {
    static const QStringList exts = {QStringLiteral("mp4"),
                                     QStringLiteral("webm")};
    return exts;
}
// Canonical media-kind tokens for the single-select omnibar dropdown.
// The query layer translates the exact tokens to native tags where the
// scheme is live-verified (Danbooru 2026-10-22: `animated` covers gif +
// webp + ugoira + video, `animated_gif` is real gifs only, `video` is
// mp4/webm, `sound` is audio mp4s); anything else rides as filetype:,
// which Danbooru answers server-side and other engines strip (the
// client-side ext net still filters).
inline QString mediaGifToken() {
    return QStringLiteral("filetype:") + gifLikeExts().join(u',');
}
inline QString mediaVideoToken() {
    return QStringLiteral("filetype:") + videoExts().join(u',');
}
inline QString mediaAnimatedToken() {
    return QStringLiteral("filetype:") + (gifLikeExts() + videoExts()).join(u',');
}
inline QString mediaStillsToken() {
    return QStringLiteral("-") + mediaAnimatedToken();
}
inline QString mediaSoundToken() {
    return QStringLiteral("filetype:mp4");
}
// Danbooru-native expansion of an exact canonical token (empty when the
// token is not a canonical media kind: custom/typed filetype: passes
// through untouched, negations included).
inline QStringList danbooruMediaTags(const QString &tok) {
    const QString low = tok.toLower();
    if (low == mediaAnimatedToken())
        return {QStringLiteral("animated")};
    if (low == mediaGifToken())
        return {QStringLiteral("animated"), QStringLiteral("-video")};
    if (low == mediaVideoToken())
        return {QStringLiteral("video")};
    if (low == mediaStillsToken())
        return {QStringLiteral("-animated"), QStringLiteral("-video")};
    return {};
}
// `filetype:a,b,c` (leading `-` tolerated): Danbooru-style media filter
// tags. The ext list is comma-OR; the token matches post extensions.
inline bool isFiletypeToken(const QString &tok) {
    QString low = tok.toLower();
    if (low.startsWith(u'-'))
        low = low.mid(1);
    return low.startsWith(QStringLiteral("filetype:"));
}
inline QSet<QString> filetypeExtSet(const QString &tok) {
    QString t = tok.toLower();
    if (t.startsWith(u'-'))
        t = t.mid(1);
    QSet<QString> out;
    const int colon = t.indexOf(u':');
    if (colon < 0)
        return out;
    for (const QString &e : t.mid(colon + 1).split(u',')) {
        const QString c = e.trimmed();
        if (!c.isEmpty())
            out.insert(c);
    }
    return out;
}
// Post extension against filetype: token groups (AND across include
// groups, excluded by none). Empty includes pass.
inline bool filetypeExtOk(const QString &ext,
                           const QList<QSet<QString>> &inc,
                           const QList<QSet<QString>> &exc) {
    const QString e = ext.toLower();
    for (const QSet<QString> &s : inc) {
        if (!s.contains(e))
            return false;
    }
    for (const QSet<QString> &s : exc) {
        if (s.contains(e))
            return false;
    }
    return true;
}

// Szurubooru-style native category split. Instances define their own
// taxonomy: the anime-family buckets fold into the fixed five, anything
// else (meme, oc, ...) stays native in `custom` so it survives instead of
// dissolving into general. Names should already be trimmed + hygiene
// checked by the caller.
struct SzuruSplit {
    QStringList artist, character, copyright, general, meta;
    QMap<QString, QStringList> custom; // native category -> tags
};

inline void szuruPlace(SzuruSplit &s, const QString &name, const QString &cat) {
    if (cat == QStringLiteral("artist"))
        s.artist << name;
    else if (cat == QStringLiteral("character"))
        s.character << name;
    else if (cat == QStringLiteral("copyright") ||
             cat == QStringLiteral("series") || cat == QStringLiteral("studio"))
        s.copyright << name;
    else if (cat == QStringLiteral("meta"))
        s.meta << name;
    else if (cat == QStringLiteral("general") ||
             cat == QStringLiteral("default") || cat.isEmpty())
        s.general << name;
    else
        s.custom[cat] << name;
}
// Cross-entry twin key: same file bytes anywhere collapse, regardless
// of site (Tor onion vs I2P mirror rows). Empty when the post carries
// no content hash.
inline QString md5DedupeKey(const Post &post) {
    return post.md5.isEmpty() ? QString()
                              : QStringLiteral("md5#") + post.md5.toLower();
}
// 32 lowercase-or-upper hex digits (API md5s, content hashes).
inline bool isMd5Hex(const QString &s) {
    if (s.size() != 32)
        return false;
    for (const QChar &c : s) {
        const ushort u = c.unicode();
        if (!((u >= u'0' && u <= u'9') || (u >= u'a' && u <= u'f') ||
              (u >= u'A' && u <= u'F')))
            return false;
    }
    return true;
}
inline const QStringList &aiContentTags() {
    static const QStringList tags = {QStringLiteral("ai_generated"),
                                     QStringLiteral("ai_created"),
                                     QStringLiteral("ai_assisted"),
                                     QStringLiteral("ai_art")};
    return tags;
}
// Ugoira (animated illustration archives, served as .zip): treated as
// animated GIFs for classification, converted to animated webp/jxl on
// download — the zip itself is never rendered.
inline bool isUgoiraPost(const Post &post) {
    if (post.ext.compare(QStringLiteral("zip"), Qt::CaseInsensitive) == 0)
        return true;
    for (const QString &t : post.tags.split(QChar::Space, Qt::SkipEmptyParts)) {
        if (t.compare(QStringLiteral("ugoira"), Qt::CaseInsensitive) == 0)
            return true;
    }
    return false;
}
// every tag blob. Drives the grid "AI" chip; nothing else keys off it.
inline bool isAiPost(const Post &post) {
    const QString blobs[] = {post.tags,        post.tagsArtist,
                             post.tagsCharacter, post.tagsCopyright,
                             post.tagsMeta,    post.tagsGeneral,
                             post.untyped};
    for (const QString &blob : blobs) {
        for (const QString &tok : blob.split(QChar::Space, Qt::SkipEmptyParts)) {
            if (aiContentTags().contains(tok.toLower().replace(u'-', u'_')))
                return true;
        }
    }
    return false;
}

// Tag hygiene at ingestion: real tags never contain commas, semicolons or
// quotes, and their parentheses always balance (danbooru qualifiers like
// name_(series)). Anything else is a split artifact from some sidecar or
// API shape ("animated_gif,", "highres)") and must not enter posts,
// indexes, favourites, or suggestions.
inline bool tagTokenOk(const QString &t) {
    if (t.isEmpty() || t.size() > 128)
        return false;
    if (t.contains(u',') || t.contains(u';') || t.contains(u'"') ||
        t.contains(u'\''))
        return false;
    int depth = 0;
    for (const QChar c : t) {
        if (c == u'(') {
            depth++;
        } else if (c == u')') {
            if (--depth < 0)
                return false;
        }
    }
    return depth == 0;
}

inline QStringList cleanTagTokens(const QStringList &tags) {
    // Trim once, keep the survivors: a transform+filter pipeline instead
    // of the hand-rolled loop (which trimmed twice per tag).
    auto good = tags | std::views::transform(
                           [](const QString &t) { return t.trimmed(); }) |
                std::views::filter(tagTokenOk);
    QStringList out;
    out.reserve(tags.size());
    for (const QString &s : good)
        out << s;
    return out;
}
