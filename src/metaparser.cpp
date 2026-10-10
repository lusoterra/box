#include "metaparser.h"
#include "explicittags.h"

#include <QJsonArray>
#include <QJsonValue>

namespace {

qint64 toInt(const QJsonValue &v, qint64 def = 0) {
    if (v.isDouble())
        return static_cast<qint64>(v.toDouble());
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
                        const QStringList &artist, const QStringList &character,
                        const QStringList &copyright, const QStringList &general,
                        const QStringList &meta) {
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
        QString gen = post.tagsGeneral;
        for (const QString &t : splitTags(combined)) {
            if (!gen.split(QChar::Space, Qt::SkipEmptyParts).contains(t))
                gen += u' ' + t;
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

bool MetaParser::parseGelbooruNative(const QJsonObject &raw, Post &post) {
    // Real gallery-dl --write-info-json output for gelbooru.
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
