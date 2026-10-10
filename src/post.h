#pragma once

#include <QString>

// Read-only post record. Mirrors the fields the Python prototypes exposed
// over /api/posts so filtering/sorting behaves the same.
struct Post {
    QString file;   // relative path with '/' separators
    QString absFile; // absolute path (for loading)
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
};

inline bool isVideoExt(const QString &ext) {
    return ext == QStringLiteral("mp4") || ext == QStringLiteral("webm");
}
