#pragma once

#include <QString>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QSet>
#include <QStringList>

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
    QJsonObject rawJson; // original API post object (written as download sidecar)
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
// every tag blob. Drives the grid "AI" chip; nothing else keys off it.
inline const QStringList &aiContentTags() {
    static const QStringList tags = {QStringLiteral("ai_generated"),
                                     QStringLiteral("ai_created"),
                                     QStringLiteral("ai_assisted"),
                                     QStringLiteral("ai_art")};
    return tags;
}
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
    QStringList out;
    out.reserve(tags.size());
    for (const QString &t : tags) {
        const QString s = t.trimmed();
        if (tagTokenOk(s))
            out << s;
    }
    return out;
}
