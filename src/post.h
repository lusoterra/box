#pragma once

#include <QString>
#include <QJsonObject>
#include <QSet>

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

// AI-generated marker: token-exact (case-insensitive, '-' == '_') across
// every tag blob. Drives the grid "AI" chip; nothing else keys off it.
inline bool isAiPost(const Post &post) {
    static const QSet<QString> ai = {QStringLiteral("ai_generated"),
                                     QStringLiteral("ai_created"),
                                     QStringLiteral("ai_assisted"),
                                     QStringLiteral("ai_art")};
    const QString blobs[] = {post.tags,        post.tagsArtist,
                             post.tagsCharacter, post.tagsCopyright,
                             post.tagsMeta,    post.tagsGeneral,
                             post.untyped};
    for (const QString &blob : blobs) {
        for (const QString &tok : blob.split(QChar::Space, Qt::SkipEmptyParts)) {
            if (ai.contains(tok.toLower().replace(u'-', u'_')))
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
