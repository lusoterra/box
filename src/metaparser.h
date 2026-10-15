#pragma once

#include "post.h"

#include <QJsonObject>
#include <QString>

// Tries every known sidecar metadata format and fills `post` (except
// file/absFile/ext/category, which the scanner sets). Returns true when a
// sidecar was parsed, false when the post has no usable metadata.
class MetaParser {
public:
    // raw is the parsed sidecar JSON object.
    static bool parse(const QJsonObject &raw, Post &post);

private:
    static bool parseLosn(const QJsonObject &raw, Post &post);
    static bool parseGalleryDlFlat(const QJsonObject &raw, Post &post);
    static bool parseDanbooruNative(const QJsonObject &raw, Post &post);
    static bool parseGelbooruNative(const QJsonObject &raw, Post &post);
    static bool parseSzuruNative(const QJsonObject &raw, Post &post);
};

// Merge a freshly fetched API object into an existing sidecar file,
// preserving its format (LOSN sidecars get re-typed tag arrays, native ones
// are replaced wholesale). Returns false when the sidecar shape is unknown.
bool writeRefreshedSidecar(const QString &sidecarPath, const QJsonObject &fresh,
                           const QString &siteKey);
