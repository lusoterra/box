#include "metaparser.h"
#include "explicittags.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QSaveFile>
#include <QSet>

#include <cmath>

namespace {

qint64 toInt(const QJsonValue &v, qint64 def = 0) {
    if (v.isDouble()) {
        // Clamp: out-of-range double->integer casts are UB (mirrors
        // BooruEngineOps::jsonInt).
        const double d = v.toDouble();
        if (!std::isfinite(d) || d > 9.0e18 || d < -9.0e18)
            return def;
        return static_cast<qint64>(d);
    }
    if (v.isString()) {
        bool ok = false;
        qint64 n = v.toString().trimmed().toLongLong(&ok);
        return ok ? n : def;
    }
    if (v.isBool())
        return v.toBool() ? 1 : 0;
    return def;
}

QStringList splitTags(const QString &s) {
    return s.split(QChar::Space, Qt::SkipEmptyParts);
}

QString firstSource(const QJsonValue &v) {
    if (v.isArray()) {
        for (const QJsonValue &e : v.toArray()) {
            if (e.isString() && !e.toString().isEmpty())
                return e.toString();
        }
        return {};
    }
    if (v.isString())
        return v.toString();
    return {};
}

void applyRatingAndTags(Post &post, const QString &rawRating,
                        const QStringList &artistIn, const QStringList &characterIn,
                        const QStringList &copyrightIn, const QStringList &generalIn,
                        const QStringList &metaIn) {
    // Hygiene first: every sidecar/API shape funnels through here.
    const QStringList artist = cleanTagTokens(artistIn);
    const QStringList character = cleanTagTokens(characterIn);
    const QStringList copyright = cleanTagTokens(copyrightIn);
    const QStringList general = cleanTagTokens(generalIn);
    const QStringList meta = cleanTagTokens(metaIn);
    QStringList all;
    all << artist << character << copyright << general << meta;
    auto [rating, inferred] = inferExplicit(all, normalizeRating(rawRating));
    post.rating = rating;
    post.ratingInferred = inferred;
    post.tags = all.join(u' ');
    post.tagsArtist = artist.join(u' ');
    post.tagsCharacter = character.join(u' ');
    post.tagsCopyright = copyright.join(u' ');
    post.tagsGeneral = general.join(u' ');
    post.tagsMeta = meta.join(u' ');
}

} // namespace

bool MetaParser::parse(const QJsonObject &raw, Post &post) {
    // Stray sidecars first: marker + flat tag string, no id/rating. Without
    // this they fall into the flat parser, whose tags never reach untyped
    // and can never be categorized.
    if (raw.value(QStringLiteral("stray")).toBool())
        return parseStray(raw, post);
    // LOSN / testfile format: tags is an array of {fullString, tagType},
    // identified by serverId/postURL markers.
    if (raw.contains(QStringLiteral("serverId")) || raw.contains(QStringLiteral("postURL")) ||
        raw.contains(QStringLiteral("md5String")))
        return parseLosn(raw, post);
    // gallery-dl native danbooru info.json uses tag_string_* fields.
    if (raw.contains(QStringLiteral("tag_string")) || raw.contains(QStringLiteral("tag_string_general")))
        return parseDanbooruNative(raw, post);
    // gallery-dl native gelbooru info.json: id/score/rating + "tags" string.
    if (raw.contains(QStringLiteral("tags")) && raw.value(QStringLiteral("tags")).isString() &&
        (raw.contains(QStringLiteral("id")) || raw.contains(QStringLiteral("rating"))))
        return parseGelbooruNative(raw, post);
    // Szurubooru native object (our own download sidecars included): "tags"
    // is an array of {names, category}.
    if (raw.value(QStringLiteral("tags")).isArray()) {
        const QJsonArray arr = raw.value(QStringLiteral("tags")).toArray();
        if (!arr.isEmpty() && arr.first().isObject() &&
            arr.first().toObject().contains(QStringLiteral("names")))
            return parseSzuruNative(raw, post);
    }
    // Fallback: flat gallery-dl layout: id/score/rating/tags[_*] keys.
    return parseGalleryDlFlat(raw, post);
}

bool MetaParser::parseLosn(const QJsonObject &raw, Post &post) {
    QStringList artist, character, copyright, general, meta, untyped;
    const QJsonValue tagsV = raw.value(QStringLiteral("tags"));
    if (tagsV.isString()) {
        general = splitTags(tagsV.toString());
    } else if (tagsV.isArray()) {
        for (const QJsonValue &e : tagsV.toArray()) {
            if (e.isObject()) {
                const QJsonObject o = e.toObject();
                const QString name = o.value(QStringLiteral("fullString")).toString().trimmed();
                if (name.isEmpty())
                    continue;
                const QString t = o.value(QStringLiteral("tagType")).toString().trimmed().toLower();
                if (t == QStringLiteral("artist"))
                    artist << name;
                else if (t == QStringLiteral("character"))
                    character << name;
                else if (t == QStringLiteral("copyright"))
                    copyright << name;
                else if (t == QStringLiteral("meta"))
                    meta << name;
                else if (t == QStringLiteral("general"))
                    general << name;
                else {
                    if (!tagTokenOk(name))
                        continue; // split artifact, not a tag
                    general << name; // "none" / unknown: tentatively general
                    untyped << name;
                }
            } else if (e.isString() && !e.toString().trimmed().isEmpty()) {
                general << e.toString().trimmed();
            }
        }
    }

    post.id = toInt(raw.value(QStringLiteral("serverId")));
    post.score = static_cast<int>(toInt(raw.value(QStringLiteral("score"))));
    applyRatingAndTags(post, raw.value(QStringLiteral("rating")).toString(),
                       artist, character, copyright, general, meta);
    post.untyped = untyped.join(u' ');
    post.width = static_cast<int>(toInt(raw.value(QStringLiteral("width"))));
    post.height = static_cast<int>(toInt(raw.value(QStringLiteral("height"))));
    post.source = firstSource(raw.value(QStringLiteral("sources")));
    const QJsonValue dateV = raw.value(QStringLiteral("postDate"));
    post.date = dateV.isString() ? dateV.toString() : QString();
    post.postUrl = raw.value(QStringLiteral("postURL")).toString();
    post.title = QString();
    if (raw.contains(QStringLiteral("category")))
        post.category = raw.value(QStringLiteral("category")).toString();
    post.hasMeta = true;
    return true;
}

bool MetaParser::parseGalleryDlFlat(const QJsonObject &raw, Post &post) {
    post.id = toInt(raw.value(QStringLiteral("id")));
    post.score = static_cast<int>(toInt(raw.value(QStringLiteral("score"))));
    applyRatingAndTags(post, raw.value(QStringLiteral("rating")).toString(),
                       splitTags(raw.value(QStringLiteral("tags_artist")).toString()),
                       splitTags(raw.value(QStringLiteral("tags_character")).toString()),
                       splitTags(raw.value(QStringLiteral("tags_copyright")).toString()),
                       splitTags(raw.value(QStringLiteral("tags_general")).toString()),
                       splitTags(raw.value(QStringLiteral("tags_metadata")).toString()));
    // Some flat files also carry a combined "tags" string; merge it into general.
    const QString combined = raw.value(QStringLiteral("tags")).toString().trimmed();
    if (!combined.isEmpty()) {
        // Set membership, not re-split per token (quadratic hang on blobs
        // with tens of thousands of tags).
        QSet<QString> seen =
            QSet<QString>(splitTags(post.tagsGeneral).begin(),
                         splitTags(post.tagsGeneral).end());
        QString gen = post.tagsGeneral;
        for (const QString &t : splitTags(combined)) {
            if (!seen.contains(t)) {
                seen.insert(t);
                gen += u' ' + t;
            }
        }
        post.tagsGeneral = gen.trimmed();
        post.tags = (post.tagsArtist + u' ' + post.tagsCharacter + u' ' + post.tagsCopyright +
                     u' ' + post.tagsGeneral + u' ' + post.tagsMeta)
                        .simplified();
        auto [rating, inferred] =
            inferExplicit(post.tags.split(QChar::Space, Qt::SkipEmptyParts), post.rating);
        post.rating = rating;
        post.ratingInferred = post.ratingInferred || inferred;
    }
    post.width = static_cast<int>(toInt(raw.value(QStringLiteral("width"))));
    post.height = static_cast<int>(toInt(raw.value(QStringLiteral("height"))));
    post.source = raw.value(QStringLiteral("source")).toString();
    post.date = raw.value(QStringLiteral("date")).toString();
    post.title = raw.value(QStringLiteral("title")).toString();
    if (raw.contains(QStringLiteral("category")))
        post.category = raw.value(QStringLiteral("category")).toString();
    post.hasMeta = true;
    return true;
}

bool MetaParser::parseDanbooruNative(const QJsonObject &raw, Post &post) {
    // Real gallery-dl --write-info-json output for danbooru.
    post.id = toInt(raw.value(QStringLiteral("id")));
    QJsonValue scoreV = raw.value(QStringLiteral("score"));
    if (scoreV.isObject())
        scoreV = scoreV.toObject().value(QStringLiteral("total"));
    post.score = static_cast<int>(toInt(scoreV));
    const QString ratingRaw = raw.value(QStringLiteral("rating")).toString();
    QStringList artist = splitTags(raw.value(QStringLiteral("tag_string_artist")).toString());
    QStringList character = splitTags(raw.value(QStringLiteral("tag_string_character")).toString());
    QStringList copyright = splitTags(raw.value(QStringLiteral("tag_string_copyright")).toString());
    QStringList general = splitTags(raw.value(QStringLiteral("tag_string_general")).toString());
    QStringList meta = splitTags(raw.value(QStringLiteral("tag_string_meta")).toString());
    if (general.isEmpty() && !raw.value(QStringLiteral("tag_string")).toString().isEmpty())
        general = splitTags(raw.value(QStringLiteral("tag_string")).toString());
    applyRatingAndTags(post, ratingRaw, artist, character, copyright, general, meta);
    post.width = static_cast<int>(toInt(raw.value(QStringLiteral("image_width"))));
    if (!post.width)
        post.width = static_cast<int>(toInt(raw.value(QStringLiteral("width"))));
    post.height = static_cast<int>(toInt(raw.value(QStringLiteral("image_height"))));
    if (!post.height)
        post.height = static_cast<int>(toInt(raw.value(QStringLiteral("height"))));
    post.source = raw.value(QStringLiteral("source")).toString();
    if (post.source.isEmpty())
        post.source = firstSource(raw.value(QStringLiteral("sources")));
    post.date = raw.value(QStringLiteral("created_at")).toString();
    post.title = raw.value(QStringLiteral("tag_string_artist")).toString().split(
                                                               QChar::Space, Qt::SkipEmptyParts)
                     .value(0);
    if (raw.contains(QStringLiteral("category")))
        post.category = raw.value(QStringLiteral("category")).toString();
    post.hasMeta = true;
    return true;
}

bool MetaParser::parseSzuruNative(const QJsonObject &raw, Post &post) {
    // Same bucket mapping as the live Szurubooru objects (see remotesource).
    SzuruSplit split;
    for (const QJsonValue &tv : raw.value(QStringLiteral("tags")).toArray()) {
        if (!tv.isObject())
            continue;
        const QJsonObject t = tv.toObject();
        QString name;
        for (const QJsonValue &n : t.value(QStringLiteral("names")).toArray()) {
            if (n.isString() && !n.toString().isEmpty()) {
                name = n.toString();
                break;
            }
        }
        // Trimmed + hygiene-gated like every other shape: untrimmed names
        // split tag matching downstream (" x" never equals "x").
        name = name.trimmed();
        if (name.isEmpty() || !tagTokenOk(name))
            continue;
        szuruPlace(split, name,
                   t.value(QStringLiteral("category")).toString().toLower());
    }
    QJsonObject rated = raw;
    const QString safety = raw.value(QStringLiteral("safety")).toString().toLower();
    rated[QStringLiteral("rating")] =
        safety == QStringLiteral("unsafe")
            ? QStringLiteral("explicit")
            : (safety == QStringLiteral("sketchy") ||
               safety == QStringLiteral("questionable"))
                  ? QStringLiteral("questionable")
                  : (safety == QStringLiteral("sensitive") ? QStringLiteral("sensitive")
                                                          : QStringLiteral("general"));
    applyRatingAndTags(post, rated.value(QStringLiteral("rating")).toString(),
                       split.artist, split.character, split.copyright,
                       split.general, split.meta);
    // Native custom categories ride along: searchable via tags, shown as
    // their own groups, never folded or classified away.
    post.customTags = split.custom;
    for (auto it = split.custom.begin(); it != split.custom.end(); ++it)
        post.tags += u' ' + it.value().join(u' ');
    post.tags = post.tags.simplified();
    post.width = static_cast<int>(toInt(raw.value(QStringLiteral("canvasWidth"))));
    if (!post.width)
        post.width = static_cast<int>(toInt(raw.value(QStringLiteral("width"))));
    post.height = static_cast<int>(toInt(raw.value(QStringLiteral("canvasHeight"))));
    if (!post.height)
        post.height = static_cast<int>(toInt(raw.value(QStringLiteral("height"))));
    post.source = raw.value(QStringLiteral("source")).toString();
    post.date = raw.value(QStringLiteral("creationTime")).toString();
    post.id = toInt(raw.value(QStringLiteral("id")));
    post.score = static_cast<int>(toInt(raw.value(QStringLiteral("score"))));
    post.hasMeta = true;
    return true;
}

bool MetaParser::parseStray(const QJsonObject &raw, Post &post) {
    const QStringList tags = cleanTagTokens(splitTags(raw.value(QStringLiteral("tags")).toString()));
    applyRatingAndTags(post, QString(), {}, {}, {}, tags, {});
    post.untyped = tags.join(u' ');
    post.width = static_cast<int>(toInt(raw.value(QStringLiteral("width"))));
    post.height = static_cast<int>(toInt(raw.value(QStringLiteral("height"))));
    post.source = raw.value(QStringLiteral("source")).toString();
    post.hasMeta = true;
    return true;
}

bool MetaParser::parseGelbooruNative(const QJsonObject &raw, Post &post) {    // Real gallery-dl --write-info-json output for gelbooru.
    post.id = toInt(raw.value(QStringLiteral("id")));
    post.score = static_cast<int>(toInt(raw.value(QStringLiteral("score"))));
    QStringList general = splitTags(raw.value(QStringLiteral("tags")).toString());
    applyRatingAndTags(post, raw.value(QStringLiteral("rating")).toString(),
                       {}, {}, {}, general, {});
    post.untyped = general.join(u' '); // one flat blob: all tentative
    post.width = static_cast<int>(toInt(raw.value(QStringLiteral("width"))));
    post.height = static_cast<int>(toInt(raw.value(QStringLiteral("height"))));
    post.source = raw.value(QStringLiteral("source")).toString();
    post.date = raw.value(QStringLiteral("created_at")).toString();
    if (raw.contains(QStringLiteral("category")))
        post.category = raw.value(QStringLiteral("category")).toString();
    post.hasMeta = true;
    return true;
}

namespace {
// Buckets from a fresh API object, by site family. Only buckets + score +
// rating are used (URLs are irrelevant for sidecar merges).
struct FreshMeta {
    QStringList artist, character, copyright, general, meta, untypedBlob;
    int score = 0;
    QString rating;
};

QStringList splitWs(const QString &s) {
    return s.split(QChar::Space, Qt::SkipEmptyParts);
}

FreshMeta freshFrom(const QJsonObject &o, const QString &siteKey) {
    FreshMeta m;
    auto getScore = [&] {
        QJsonValue v = o.value(QStringLiteral("score"));
        if (v.isObject())
            v = v.toObject().value(QStringLiteral("total"));
        if (v.isDouble())
            return static_cast<int>(v.toDouble());
        if (v.isString()) {
            bool ok = false;
            const int n = v.toString().trimmed().toInt(&ok);
            return ok ? n : 0;
        }
        return 0;
    };
    m.score = getScore();
    if (siteKey == QStringLiteral("danbooru")) {
        m.artist = splitWs(o.value(QStringLiteral("tag_string_artist")).toString());
        m.character = splitWs(o.value(QStringLiteral("tag_string_character")).toString());
        m.copyright = splitWs(o.value(QStringLiteral("tag_string_copyright")).toString());
        m.general = splitWs(o.value(QStringLiteral("tag_string_general")).toString());
        if (m.general.isEmpty())
            m.general = splitWs(o.value(QStringLiteral("tag_string")).toString());
        m.meta = splitWs(o.value(QStringLiteral("tag_string_meta")).toString());
        m.rating = o.value(QStringLiteral("rating")).toString();
    } else if (siteKey == QStringLiteral("szurubooru")) {
        for (const QJsonValue &tv : o.value(QStringLiteral("tags")).toArray()) {
            if (!tv.isObject())
                continue;
            const QJsonObject t = tv.toObject();
            QString name;
            for (const QJsonValue &n : t.value(QStringLiteral("names")).toArray()) {
                if (n.isString() && !n.toString().isEmpty()) {
                    name = n.toString();
                    break;
                }
            }
            name = name.trimmed();
            if (name.isEmpty() || !tagTokenOk(name))
                continue;
            const QString cat = t.value(QStringLiteral("category")).toString().toLower();
            if (cat == QStringLiteral("artist"))
                m.artist << name;
            else if (cat == QStringLiteral("character"))
                m.character << name;
            else if (cat == QStringLiteral("copyright") || cat == QStringLiteral("series") ||
                     cat == QStringLiteral("studio"))
                m.copyright << name;
            else if (cat == QStringLiteral("meta"))
                m.meta << name;
            else
                m.general << name;
        }
        const QString safety = o.value(QStringLiteral("safety")).toString().toLower();
        if (safety == QStringLiteral("unsafe"))
            m.rating = QStringLiteral("explicit");
        else if (safety == QStringLiteral("sketchy") ||
                 safety == QStringLiteral("questionable"))
            m.rating = QStringLiteral("questionable");
        else if (safety == QStringLiteral("sensitive"))
            m.rating = QStringLiteral("sensitive");
        else
            m.rating = QStringLiteral("general");
    } else {
        // Gelbooru family + Moebooru: one flat blob.
        m.untypedBlob = splitWs(o.value(QStringLiteral("tags")).toString());
        m.rating = o.value(QStringLiteral("rating")).toString();
    }
    return m;
}
} // namespace

bool writeRefreshedSidecar(const QString &sidecarPath, const QJsonObject &fresh,
                           const QString &siteKey) {
    QFile f(sidecarPath);
    static constexpr qint64 kMaxSidecar = 8LL * 1024 * 1024;
    if (f.size() > kMaxSidecar)
        return false;
    if (!f.open(QIODevice::ReadOnly))
        return false;
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
    f.close();
    if (err.error != QJsonParseError::NoError || !doc.isObject())
        return false;
    QJsonObject cur = doc.object();
    const bool isLosn = cur.contains(QStringLiteral("serverId")) ||
                        cur.contains(QStringLiteral("postURL")) ||
                        cur.contains(QStringLiteral("md5String"));
    const bool isDanNative = cur.contains(QStringLiteral("tag_string")) ||
                             cur.contains(QStringLiteral("tag_string_general"));
    const bool isBlobNative =
        cur.contains(QStringLiteral("tags")) && cur.value(QStringLiteral("tags")).isString();

    const FreshMeta m = freshFrom(fresh, siteKey);
    // Atomic replace (QSaveFile tmp+rename, full-write checked): a kill
    // between truncate and write used to leave 0B sidecars behind,
    // which Syncthing-style mirrors then flag as local additions.
    auto atomicReplace = [&](const QJsonDocument &doc) {
        QSaveFile f(sidecarPath);
        if (!f.open(QIODevice::WriteOnly))
            return false;
        const QByteArray bytes = doc.toJson(QJsonDocument::Indented); // jq-readable
        if (f.write(bytes) != bytes.size()) {
            f.cancelWriting();
            return false;
        }
        return f.commit();
    };
    if (isLosn) {
        // Rebuild the tag array with real types; blob tags stay "none"
        // (reclassifiable), split tags take their bucket names.
        QJsonArray tags;
        auto push = [&](const QStringList &names, const char *type) {
            for (const QString &n : names) {
                QJsonObject t;
                t[QStringLiteral("fullString")] = n;
                t[QStringLiteral("tagType")] = QString::fromLatin1(type);
                tags.append(t);
            }
        };
        push(m.artist, "artist");
        push(m.character, "character");
        push(m.copyright, "copyright");
        push(m.general, "general");
        push(m.meta, "meta");
        push(m.untypedBlob, "none");
        cur[QStringLiteral("tags")] = tags;
        cur[QStringLiteral("score")] = QString::number(m.score);
        cur[QStringLiteral("rating")] = normalizeRating(m.rating);
        return atomicReplace(QJsonDocument(cur));
    }
    if (isDanNative || isBlobNative) {
        // Same shape, fresher data: wholesale replacement.
        return atomicReplace(QJsonDocument(fresh));
    }
    return false;
}
