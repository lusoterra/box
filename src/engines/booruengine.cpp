// BooruEngineOps base: quirk-aware API root, shared mapping helpers,
// engine taxonomy and other pure helpers. Per-family request/response
// shapes live in engine_danbooru.cpp and friends.
#include "booruengine.h"
#include "explicittags.h"
#include "tagcategories.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <QUrlQuery>
#include <climits>
#include <cmath>

QString BooruEngineOps::apiBase(const Context &ctx) const {
    return RemotePosts::apiBaseUrl(ctx.engine, ctx.baseUrl);
}

QString BooruEngineOps::joinTags(const QStringList &tags) {
    QStringList clean;
    for (const QString &t : tags) {
        const QString s = t.trimmed();
        if (!s.isEmpty())
            clean << s;
    }
    return clean.join(u' ');
}

qint64 BooruEngineOps::jsonInt(const QJsonValue &v, qint64 def) {
    if (v.isDouble()) {
        // Clamp: out-of-range double->integer casts are UB, and a rogue
        // API object must never trigger that (score 1e30 and friends).
        const double d = v.toDouble();
        if (!std::isfinite(d) || d > 9.0e18 || d < -9.0e18)
            return def;
        return static_cast<qint64>(d);
    }
    if (v.isString()) {
        bool ok = false;
        const qint64 n = v.toString().trimmed().toLongLong(&ok);
        return ok ? n : def;
    }
    return def;
}

int BooruEngineOps::jsonInt32(const QJsonValue &v, int def) {
    static constexpr qint64 lo = qint64(INT_MIN), hi = qint64(INT_MAX);
    const qint64 n = jsonInt(v, qint64(def));
    if (n < lo || n > hi)
        return def;
    return int(n);
}

QStringList BooruEngineOps::splitTags(const QString &s) {
    return s.split(QChar::Space, Qt::SkipEmptyParts);
}

QString BooruEngineOps::extOf(const QString &fileUrl, const QString &fallback) {
    // Last path segment only ("a.b/c" has no extension), strict match:
    // a hostile tail like b/c falls back instead of reaching a filename.
    const QString segment = QUrl(fileUrl).path().section(u'/', -1);
    const int dot = segment.lastIndexOf(u'.');
    if (dot < 0)
        return fallback;
    static const QRegularExpression ext(
        QStringLiteral("^([A-Za-z0-9]{1,5})$"));
    const auto m = ext.match(segment.mid(dot + 1).toLower());
    return m.hasMatch() ? m.captured(1) : fallback;
}

QString BooruEngineOps::resolveFileUrl(const QString &baseUrl, const QString &url) {
    if (url.isEmpty() || url.startsWith(QStringLiteral("http")))
        return url;
    QString path = url;
    while (path.startsWith(u'/'))
        path.remove(0, 1);
    return baseUrl + QStringLiteral("/images/") + path;
}

QJsonDocument BooruEngineOps::parseJsonBody(const QByteArray &body) {
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &err);
    if (err.error != QJsonParseError::NoError)
        return QJsonDocument();
    return doc;
}

QJsonObject BooruEngineOps::firstOfArray(const QJsonArray &arr, bool &ok) {
    for (const QJsonValue &v : arr) {
        if (v.isObject()) {
            ok = true;
            return v.toObject();
        }
    }
    return QJsonObject();
}

namespace RemotePosts {
QString classicBucketForCategory(const QString &category) {
    return TagCategories::classicBucket(category);
}
} // namespace RemotePosts

void BooruEngineOps::finishRemote(Post &p, const QString &siteLabel,
                                  const QJsonObject &raw, BooruEngine engine,
                                  const QString &host) {
    p.isRemote = true;
    // Comment/annotation presence for the grid pills. Per-engine key
    // lists (shapes verified against fixtures + live probes): a shared
    // check across engines misfires on same-named keys with different
    // meanings, so each family reads only its own spellings.
    // Tolerant readers: APIs send counts as numbers, numeric strings or
    // bools, and flags as bools or yes/no strings.
    auto num = [](const QJsonValue &v, bool *ok) -> double {
        if (v.isDouble()) {
            if (ok)
                *ok = true;
            return v.toDouble();
        }
        if (v.isBool()) {
            if (ok)
                *ok = true;
            return v.toBool() ? 1.0 : 0.0;
        }
        if (v.isString()) {
            bool good = false;
            const double d = v.toString().trimmed().toDouble(&good);
            if (good) {
                if (ok)
                    *ok = true;
                return d;
            }
        }
        return 0.0;
    };
    auto countOf = [&](std::initializer_list<const char *> keys) -> double {
        for (const char *k : keys) {
            bool ok = false;
            const double d = num(raw.value(QString::fromLatin1(k)), &ok);
            if (ok)
                return d;
        }
        return 0.0;
    };
    auto present = [](const QJsonValue &v) {
        if (v.isNull() || v.isUndefined())
            return false;
        if (v.isBool())
            return v.toBool();
        if (v.isDouble())
            return v.toDouble() != 0.0;
        if (v.isString()) {
            const QString s = v.toString().trimmed();
            return !s.isEmpty() && s != QStringLiteral("0") &&
                   s.compare(QStringLiteral("no"),
                             Qt::CaseInsensitive) != 0;
        }
        if (v.isArray())
            return !v.toArray().isEmpty();
        if (v.isObject())
            return !v.toObject().isEmpty();
        return false;
    };
    auto flag = [&](const char *key) {
        const QJsonValue v = raw.value(QString::fromLatin1(key));
        if (v.isBool())
            return v.toBool();
        if (v.isString()) {
            const QString s = v.toString().trimmed().toLower();
            return s == QStringLiteral("yes") || s == QStringLiteral("y") ||
                   s == QStringLiteral("true") || s == QStringLiteral("1");
        }
        if (v.isDouble())
            return v.toDouble() != 0.0;
        return false;
    };
    auto stamp = [&](std::initializer_list<const char *> keys) {
        for (const char *k : keys) {
            if (present(raw.value(QString::fromLatin1(k))))
                return true;
        }
        return false;
    };
    using enum BooruEngine;
    switch (engine) {
    case Danbooru:
        p.hasComments = stamp({"last_commented_at", "last_comment_bumped_at"});
        p.hasNotes = stamp({"last_noted_at"});
        break;
    case Gelbooru:
    case Safebooru: {
        const double c = countOf({"comment_count"});
        if (c > 0) {
            p.commentCount = int(qMin<double>(c, INT_MAX));
            p.hasComments = true;
        }
        p.hasNotes = flag("has_notes");
        break;
    }
    case Moebooru:
        // Numeric epochs (0 = none) or stamp strings.
        p.hasComments = stamp({"last_commented_at"});
        p.hasNotes = stamp({"last_noted_at"});
        break;
    case Szurubooru: {
        const double c = countOf({"commentCount"});
        if (c > 0) {
            p.commentCount = int(qMin<double>(c, INT_MAX));
            p.hasComments = true;
        }
        p.hasNotes = raw.value(QStringLiteral("notes")).isArray() &&
                     !raw.value(QStringLiteral("notes")).toArray().isEmpty();
        break;
    }
    default: {
        // Unverified schemes (Shimmie, HyBooru, Nuuru):
        // numeric counts and explicit flags only, no stamp guessing.
        const double c = countOf({"comment_count", "commentCount"});
        if (c > 0) {
            p.commentCount = int(qMin<double>(c, INT_MAX));
            p.hasComments = true;
        }
        p.hasNotes = flag("has_notes") ||
                     (raw.value(QStringLiteral("notes")).isArray() &&
                      !raw.value(QStringLiteral("notes")).toArray().isEmpty());
        break;
    }
    }
    // Hygiene: every engine's tag blob funnels through here.
    p.tagsArtist = cleanTagTokens(splitTags(p.tagsArtist)).join(u' ');
    p.tagsCharacter = cleanTagTokens(splitTags(p.tagsCharacter)).join(u' ');
    p.tagsCopyright = cleanTagTokens(splitTags(p.tagsCopyright)).join(u' ');
    p.tagsGeneral = cleanTagTokens(splitTags(p.tagsGeneral)).join(u' ');
    p.tagsMeta = cleanTagTokens(splitTags(p.tagsMeta)).join(u' ');
    p.untyped = cleanTagTokens(splitTags(p.untyped)).join(u' ');
    // Native category groups that merely repeat a classic bucket (Nuuru's
    // Character/Meta/Series, HyBooru namespaces, ...) must not render a
    // second time. Drop only when every tag already survives in the
    // classic blobs, so no genuinely custom tag is lost.
    {
        QSet<QString> classic;
        for (const QString &blob : {p.tagsArtist, p.tagsCharacter,
                                    p.tagsCopyright, p.tagsGeneral,
                                    p.tagsMeta}) {
            for (const QString &t : splitTags(blob))
                classic.insert(t);
        }
        for (auto it = p.customTags.begin(); it != p.customTags.end();) {
            if (RemotePosts::classicBucketForCategory(it.key()).isEmpty()) {
                ++it;
                continue;
            }
            bool allClassic = !it.value().isEmpty();
            for (const QString &t : it.value()) {
                if (!classic.contains(t.trimmed())) {
                    allClassic = false;
                    break;
                }
            }
            if (allClassic)
                it = p.customTags.erase(it);
            else
                ++it;
        }
    }
    // Disabled buckets for this booru fold into general (non-anime
    // taxonomies). Per-host keys win; legacy engine keys still apply.
    // General itself never folds.
    auto foldBucket = [&](QString &field, const char *bucket) {
        const QString b = QString::fromLatin1(bucket);
        if (!field.isEmpty() &&
            (RemotePosts::bucketDisabledFor(host, b) ||
             RemotePosts::bucketDisabled(engine, b))) {
            p.tagsGeneral = p.tagsGeneral.isEmpty()
                                ? field
                                : p.tagsGeneral + u' ' + field;
            field.clear();
        }
    };
    foldBucket(p.tagsArtist, "artist");
    foldBucket(p.tagsCharacter, "character");
    foldBucket(p.tagsCopyright, "copyright");
    foldBucket(p.tagsMeta, "meta");
    p.siteName = siteLabel;
    p.siteShort = RemotePosts::shortFor(siteLabel);
    p.category = siteLabel; // Sources filter groups remote posts by booru
    p.hasMeta = true;
    p.rawJson = raw;
    // Content hash for cross-entry twin collapse (Tor onion vs I2P mirror
    // rows of the same file). md5 or Szuru's checksumMD5, validated hex.
    QString md5 = raw.value(QStringLiteral("md5")).toString();
    if (!isMd5Hex(md5))
        md5 = raw.value(QStringLiteral("checksumMD5")).toString();
    p.md5 = isMd5Hex(md5) ? md5.toLower() : QString();
    QStringList all;
    all << splitTags(p.tagsArtist) << splitTags(p.tagsCharacter)
        << splitTags(p.tagsCopyright) << splitTags(p.tagsGeneral)
        << splitTags(p.tagsMeta);
    auto [rating, inferred] = inferExplicit(all, p.rating);
    p.rating = rating;
    p.ratingInferred = inferred;
    p.tags = all.join(u' ');
}

namespace RemotePosts {

QString engineKey(BooruEngine engine) {
    using enum BooruEngine;
    switch (engine) {
    case Danbooru: return QStringLiteral("danbooru");
    case Gelbooru: return QStringLiteral("gelbooru");
    case Moebooru: return QStringLiteral("moebooru");
    case Szurubooru: return QStringLiteral("szurubooru");
    case Shimmie: return QStringLiteral("shimmie");
    case Hybooru: return QStringLiteral("hybooru");
    case Nuuru: return QStringLiteral("nuuru");
    case Derpibooru: return QStringLiteral("derpibooru");
    case Twibooru: return QStringLiteral("twibooru");
    case Safebooru: return QStringLiteral("gelbooru"); // dapi family
    }
    return QStringLiteral("danbooru");
}

BooruEngine engineForKey(const QString &key) {
    if (key == QStringLiteral("gelbooru"))
        return BooruEngine::Gelbooru;
    if (key == QStringLiteral("moebooru"))
        return BooruEngine::Moebooru;
    if (key == QStringLiteral("szurubooru"))
        return BooruEngine::Szurubooru;
    if (key == QStringLiteral("shimmie"))
        return BooruEngine::Shimmie;
    if (key == QStringLiteral("hybooru"))
        return BooruEngine::Hybooru;
    if (key == QStringLiteral("nuuru"))
        return BooruEngine::Nuuru;
    if (key == QStringLiteral("derpibooru") || key == QStringLiteral("philomena"))
        return BooruEngine::Derpibooru;
    if (key == QStringLiteral("twibooru"))
        return BooruEngine::Twibooru;
    return BooruEngine::Danbooru;
}

QString engineLabel(BooruEngine engine) {
    using enum BooruEngine;
    switch (engine) {
    case Danbooru: return QStringLiteral("Danbooru");
    case Gelbooru: return QStringLiteral("Gelbooru");
    case Moebooru: return QStringLiteral("Moebooru");
    case Szurubooru: return QStringLiteral("Szurubooru");
    case Shimmie: return QStringLiteral("Shimmie");
    case Hybooru: return QStringLiteral("HyBooru");
    case Nuuru: return QStringLiteral("Nuuru (Soybooru)");
    case Derpibooru: return QStringLiteral("Derpibooru");
    case Twibooru: return QStringLiteral("Twibooru");
    case Safebooru: return QStringLiteral("Safebooru");
    }
    return QStringLiteral("Danbooru");
}

// Nuuru's native sort: vocabulary, observed live in its own search UI.
static const QStringList &nuuruSortValues() {
    static const QStringList v = {QStringLiteral("date"),
                                  QStringLiteral("date_asc"),
                                  QStringLiteral("date_desc"),
                                  QStringLiteral("score"),
                                  QStringLiteral("score_asc"),
                                  QStringLiteral("score_desc"),
                                  QStringLiteral("controversial"),
                                  QStringLiteral("controversial_desc"),
                                  QStringLiteral("id"),
                                  QStringLiteral("id_asc"),
                                  QStringLiteral("id_desc"),
                                  QStringLiteral("filesize"),
                                  QStringLiteral("filesize_asc"),
                                  QStringLiteral("filesize_desc"),
                                  QStringLiteral("tagcount"),
                                  QStringLiteral("tagcount_asc"),
                                  QStringLiteral("tagcount_desc"),
                                  QStringLiteral("random")};
    return v;
}

bool isNativeNuuruSort(const QString &value) {
    return nuuruSortValues().contains(value.trimmed().toLower());
}

QStringList translateSearchTags(const QStringList &tags, BooruEngine engine) {
    // "Safe" has no universal spelling: danbooru/moebooru/shimmie take
    // rating:safe, Gelbooru only understands rating:general, Szurubooru
    // wants safety:safe (safety:unsafe for explicit). Single-letter
    // shorthands (rating:g/s/q/e, danbooru convention) expand to full
    // words first so the rules below apply to them too - otherwise a
    // multibooru rating:g only answers on engines that tolerate it.
    // Anything else passes through untouched.
    QStringList out;
    out.reserve(tags.size());
    for (QString tok : tags) {
        const int colon = tok.indexOf(u':');
        if (colon > 0 &&
            tok.left(colon).compare(QStringLiteral("rating"),
                                    Qt::CaseInsensitive) == 0) {
            const QString v =
                tok.mid(colon + 1).trimmed().toLower();
            if (v == QStringLiteral("g"))
                tok = QStringLiteral("rating:general");
            else if (v == QStringLiteral("s"))
                tok = QStringLiteral("rating:safe");
            else if (v == QStringLiteral("q"))
                tok = QStringLiteral("rating:questionable");
            else if (v == QStringLiteral("e"))
                tok = QStringLiteral("rating:explicit");
        }
        if (tok.compare(QStringLiteral("rating:safe"), Qt::CaseInsensitive) == 0) {
            if (engine == BooruEngine::Gelbooru)
                tok = QStringLiteral("rating:general");
            else if (engine == BooruEngine::Szurubooru)
                tok = QStringLiteral("safety:safe");
        } else if (tok.compare(QStringLiteral("rating:explicit"),
                               Qt::CaseInsensitive) == 0 &&
                   engine == BooruEngine::Szurubooru) {
            tok = QStringLiteral("safety:unsafe");
        } else if (tok.compare(QStringLiteral("rating:general"),
                               Qt::CaseInsensitive) == 0 &&
                   engine == BooruEngine::Szurubooru) {
            tok = QStringLiteral("safety:safe");
        }
        if (engine == BooruEngine::Nuuru) {
            // Nuuru's sort: vocabulary is native (seen live in its search
            // UI): keep validated spellings server-side; rating:/safety:/
            // order: have no verified spelling, so drop them like before.
            const int ci = tok.indexOf(u':');
            QString head = (ci > 0 ? tok.left(ci) : QString()).toLower();
            while (head.startsWith(u'-'))
                head = head.mid(1);
            if (head == QStringLiteral("rating") ||
                head == QStringLiteral("safety") ||
                head == QStringLiteral("order"))
                continue;
            if (head == QStringLiteral("sort") &&
                !isNativeNuuruSort(tok.mid(ci + 1)))
                continue;
        } else if (engine == BooruEngine::Hybooru ||
                   engine == BooruEngine::Derpibooru ||
                   engine == BooruEngine::Twibooru) {
            // Neither engine exposes a verified content-rating or sort
            // spelling (HyBooru rating: is a 0-100 star filter): such
            // tokens would silently mis-filter, so drop them - sorting
            // stays client-side, rating filters client-side.
            const int ci = tok.indexOf(u':');
            const QString head =
                (ci > 0 ? tok.left(ci) : QString()).toLower();
            if (head == QStringLiteral("rating") ||
                head == QStringLiteral("safety") ||
                head == QStringLiteral("sort") ||
                head == QStringLiteral("order"))
                continue;
        }
        if (engine != BooruEngine::Danbooru && isFiletypeToken(tok)) {
            // Only Danbooru's filetype: support is verified: elsewhere the
            // token would silently mis-filter (literal tag match, usually
            // empty), so drop it - the client-side net still filters.
            continue;
        }
        if (engine == BooruEngine::Moebooru) {
            // Bare media tags are sparse or content-tagged on Moebooru
            // (konachan's 263-post `animated`, yandere has none), so drop
            // them server-side (the query then returns the full set) and
            // let the client-side ext net in applyBrowseFilter do the
            // filtering. `ugoira` stays literal (a real cross-site tag;
            // its ext mapping is unreliable).
            // Gelbooru/Safebooru verified 2026-11-07: their `animated`/
            // `video` are real, comprehensive tags (HTML `video`/`webm`
            // lists identical, 42 on page one; posts carry both), and
            // stripping starved the client net to 1-3 videos per mixed
            // page. Danbooru keeps the native tags; unmapped engines
            // keep the literal (their schemes are unverified).
            QString bare = tok;
            if (bare.startsWith(u'-') && bare.size() > 1)
                bare = bare.mid(1);
            const QString low = bare.toLower();
            if (low == QStringLiteral("animated") ||
                low == QStringLiteral("animated_gif") ||
                low == QStringLiteral("video"))
                continue;
        }
        if (engine == BooruEngine::Danbooru) {
            // Canonical dropdown kinds ride as native tags (live-verified:
            // `animated` spans gif/webp/ugoira/video, `video` is mp4/webm).
            // Custom/typed filetype: (negations included) passes through:
            // Danbooru answers it server-side.
            const QStringList native = danbooruMediaTags(tok);
            if (!native.isEmpty()) {
                out += native;
                continue;
            }
        }
        out << tok;
    }
    return out;
}

QString sortSearchTag(const QString &sort, BooruEngine engine) {
    // Verified spellings only (cheatsheets: danbooru/help:cheatsheet,
    // gelbooru help topic=cheatsheet, konachan help/cheatsheet). Unmapped
    // engines/modes sort client-side; "added" and the id-desc default
    // need no tag anywhere.
    if (sort == QStringLiteral("id-desc")) {
        if (engine == BooruEngine::Nuuru)
            return QStringLiteral("sort:id_desc");
        return {};
    }
    if (sort == QStringLiteral("id-asc")) {
        if (engine == BooruEngine::Danbooru)
            return QStringLiteral("order:id_asc");
        if (engine == BooruEngine::Gelbooru || engine == BooruEngine::Safebooru)
            return QStringLiteral("sort:id:asc");
        if (engine == BooruEngine::Moebooru)
            return QStringLiteral("order:id");
        if (engine == BooruEngine::Nuuru)
            return QStringLiteral("sort:id_asc");
        return {};
    }
    if (sort == QStringLiteral("score-desc")) {
        if (engine == BooruEngine::Danbooru)
            return QStringLiteral("order:score");
        if (engine == BooruEngine::Gelbooru || engine == BooruEngine::Safebooru)
            return QStringLiteral("sort:score:desc");
        if (engine == BooruEngine::Nuuru)
            return QStringLiteral("sort:score_desc");
        return {};
    }
    if (sort == QStringLiteral("score-asc")) {
        if (engine == BooruEngine::Danbooru)
            return QStringLiteral("order:score_asc");
        if (engine == BooruEngine::Gelbooru || engine == BooruEngine::Safebooru)
            return QStringLiteral("sort:score:asc");
        if (engine == BooruEngine::Nuuru)
            return QStringLiteral("sort:score_asc");
        return {};
    }
    if (sort == QStringLiteral("random")) {
        if (engine == BooruEngine::Danbooru)
            return QStringLiteral("order:random");
        if (engine == BooruEngine::Nuuru)
            return QStringLiteral("sort:random");
        return {};
    }
    return {}; // "added", "id-desc" (server default), unknown
}

bool isTransientSearchError(int httpStatus, const QString &error) {
    if (error.isEmpty())
        return false;
    if (httpStatus == 408 || httpStatus == 429 || httpStatus >= 500)
        return true;
    // http 0: QNAM never reached the server (DNS, timeout, reset).
    // "cancell" excluded defensively: aborted flights disconnect first
    // and never report, but must never arm a retry if one slips through.
    return httpStatus == 0 &&
           !error.contains(QStringLiteral("cancell"), Qt::CaseInsensitive);
}

QString uniqueBooruKey(const QString &label, const QSet<QString> &taken) {
    QString base;
    for (const QChar &c : label.toLower()) {
        if (c.isLetterOrNumber())
            base += c;
    }
    if (base.isEmpty())
        base = QStringLiteral("booru");
    QString key = base;
    int n = 2;
    while (taken.contains(key))
        key = base + QString::number(n++);
    return key;
}

QStringList defaultTagsFor(const QStringList &base, const QString &defaults,
                           const QSet<QString> &dropped) {
    QStringList tags = base;
    for (const QString &tok :
         defaults.split(QChar::Space, Qt::SkipEmptyParts)) {
        if (dropped.contains(tok))
            continue;
        if (tok == QStringLiteral("furry:g")) {
            // "furry:g or equivalent": the portable form is the furry tag
            // plus the general rating filter every engine understands.
            tags << QStringLiteral("furry") << QStringLiteral("rating:general");
        } else {
            tags << tok;
        }
    }
    return tags;
}

QString shortFor(const QString &siteLabel) {
    if (siteLabel == QStringLiteral("Danbooru"))
        return QStringLiteral("DAN");
    if (siteLabel == QStringLiteral("Gelbooru"))
        return QStringLiteral("GEL");
    if (siteLabel == QStringLiteral("Safebooru"))
        return QStringLiteral("SFW");
    if (siteLabel == QStringLiteral("Yandere"))
        return QStringLiteral("YAN");
    if (siteLabel == QStringLiteral("Konachan"))
        return QStringLiteral("KON");
    if (siteLabel == QStringLiteral("Shimmie"))
        return QStringLiteral("SHI");
    return siteLabel.left(3).toUpper();
}

QString thumbUrlFor(const Post &p) {
    if (!p.previewUrl.isEmpty())
        return p.previewUrl;
    if (!p.sampleUrl.isEmpty())
        return p.sampleUrl;
    return p.fileUrl;
}

QString viewerUrlFor(const Post &p) {
    if (!p.sampleUrl.isEmpty())
        return p.sampleUrl;
    return p.fileUrl;
}

QVector<QPair<QString, QString>>
reverseSearchUrls(const QString &fileUrl) {
    const QUrl file(fileUrl);
    if (fileUrl.isEmpty() || !isHttpUrl(fileUrl) || !file.isValid())
        return {};
    QVector<QPair<QString, QString>> out;
    const auto withUrl = [&](const QString &base,
                             const QList<QPair<QString, QString>> &params) {
        QUrl url(base);
        QUrlQuery query;
        for (const auto &[k, v] : params)
            query.addQueryItem(k, v);
        query.addQueryItem(QStringLiteral("url"), fileUrl);
        url.setQuery(query);
        return url.toString(QUrl::FullyEncoded);
    };
    out.append({QStringLiteral("SauceNAO"),
                withUrl(QStringLiteral("https://saucenao.com/search.php"), {})});
    out.append({QStringLiteral("Yandex"),
                withUrl(QStringLiteral("https://yandex.com/images/search"),
                        {{"rpt", "imageview"}})});
    out.append({QStringLiteral("Google Lens"),
                withUrl(QStringLiteral("https://lens.google.com/uploadbyurl"),
                        {})});
    out.append({QStringLiteral("IQDB"),
                withUrl(QStringLiteral("https://iqdb.org/"), {})});
    out.append({QStringLiteral("trace.moe"),
                withUrl(QStringLiteral("https://trace.moe/"), {})});
    return out;
}

bool isImageUrl(const QString &url) {
    const QString ext = BooruEngineOps::extOf(url, QString());
    return ext == QStringLiteral("jpg") || ext == QStringLiteral("jpeg") ||
           ext == QStringLiteral("png") || ext == QStringLiteral("gif") ||
           ext == QStringLiteral("webp") || ext == QStringLiteral("avif") ||
           ext == QStringLiteral("bmp");
}

QString postPageUrl(const QString &siteKey, const QString &baseUrl, qint64 id) {
    if (siteKey == QStringLiteral("danbooru") || siteKey == QStringLiteral("hybooru"))
        return baseUrl + QStringLiteral("/posts/") + QString::number(id);
    if (siteKey == QStringLiteral("twibooru"))
        return baseUrl + QStringLiteral("/posts/") + QString::number(id);
    if (siteKey == QStringLiteral("yandere") || siteKey == QStringLiteral("konachan"))
        return baseUrl + QStringLiteral("/post/show/") + QString::number(id);
    if (siteKey == QStringLiteral("szurubooru")) {
        // Per-host post shapes: most instances sit under /post/<id>, but
        // the root-id hosts (yosho.io and family -> /<id> directly) route
        // the post at the origin root. Host of the instance decides;
        // baseUrl may already carry a subpath (visuabusters lives under
        // /booru and is entered as such, so /booru/post/<id> falls out).
        QString host = QUrl(baseUrl).host().toLower();
        if (host.startsWith(QStringLiteral("www.")))
            host = host.mid(4);
        static const QSet<QString> rootId = {
            QStringLiteral("yosho.io"), QStringLiteral("jej.lol"),
            QStringLiteral("boymoders.com"), QStringLiteral("bury.pink"),
            QStringLiteral("flube.supply"), QStringLiteral("spikedog.school"),
        };
        if (rootId.contains(host)) {
            QString b = baseUrl;
            while (b.endsWith(u'/') && b.size() > 7) b.chop(1);
            return b + u'/' + QString::number(id);
        }
        return baseUrl + QStringLiteral("/post/") + QString::number(id);
    }
    if (siteKey == QStringLiteral("shimmie") || siteKey == QStringLiteral("nuuru"))
        return baseUrl + QStringLiteral("/post/view/") + QString::number(id);
    if (siteKey == QStringLiteral("derpibooru"))
        return baseUrl + QStringLiteral("/images/") + QString::number(id);
    return baseUrl + QStringLiteral("/index.php?page=post&s=view&id=") +
           QString::number(id);
}

QString displayFileName(const Post &post) {
    const QString rawName = QUrl(post.fileUrl).fileName();
    if (rawName.contains(u'.'))
        return rawName; // real filename (even extensionless-looking ones keep theirs)
    // Extensionless endpoint (Nuuru /file): synthesize something readable.
    QString host = QUrl(post.fileUrl).host().toLower();
    if (host.startsWith(QStringLiteral("www.")))
        host = host.mid(4);
    QStringList kept;
    int budget = 100;
    for (const QString &t :
         post.tags.split(QChar::Space, Qt::SkipEmptyParts)) {
        if (t.size() + 1 > budget)
            break;
        kept << t;
        budget -= t.size() + 1;
    }
    QString name;
    if (post.id > 0)
        name += QString::number(post.id) + QStringLiteral(" - ");
    if (!host.isEmpty())
        name += host + QStringLiteral(" - ");
    name += kept.join(u' ');
    name = name.trimmed();
    if (name.endsWith(u'-'))
        name.chop(1);
    name = name.trimmed();
    if (!post.ext.isEmpty())
        name += u'.' + post.ext;
    return name;
}

QString downloadFileName(const QString &siteLabel, const QJsonObject &raw) {
    QString safe = siteLabel;
    // A leading underscore survives (the _Stray convention); anything
    // else non-alphanumeric goes.
    const bool lead = safe.startsWith(u'_');
    safe.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9]+")), QString());
    if (lead && !safe.startsWith(u'_'))
        safe.prepend(u'_');
    if (safe.isEmpty())
        safe = QStringLiteral("Booru");
    QString ext = raw.value(QStringLiteral("file_ext")).toString().toLower();
    // Leading alphanumerics only (same traversal rule as extOf): a
    // hostile file_ext like "jpg/../../x" degrades to "jpg", anything
    // unusable falls through to the URL derivation below.
    static const QRegularExpression leadExt(
        QStringLiteral("^([A-Za-z0-9]{1,5})"));
    const auto m = leadExt.match(ext);
    ext = m.hasMatch() ? m.captured(1) : QString();
    if (ext.isEmpty()) {
        const QString url = raw.value(QStringLiteral("file_url")).toString();
        if (!url.isEmpty())
            ext = BooruEngineOps::extOf(url);
    }
    if (ext.isEmpty())
        ext = QStringLiteral("jpg");
    // Content hashes are hex by definition: anything else (paths,
    // javascript:, "../..") falls back to the numeric id. Never let a
    // remote string choose the filename.
    auto validMd5 = [](const QString &s) {
        if (s.size() != 32)
            return false;
        for (const QChar &c : s) {
            const ushort u = c.unicode();
            if (!((u >= u'0' && u <= u'9') || (u >= u'a' && u <= u'f') ||
                  (u >= u'A' && u <= u'F')))
                return false;
        }
        return true;
    };
    QString md5 = raw.value(QStringLiteral("md5")).toString();
    if (!validMd5(md5))
        md5 = raw.value(QStringLiteral("checksumMD5")).toString(); // szurubooru
    if (!validMd5(md5))
        md5.clear();
    if (!md5.isEmpty())
        return safe + u'_' + md5.toLower() + u'.' + ext;
    return safe + u'_' + QString::number(BooruEngineOps::jsonInt(raw.value(QStringLiteral("id")))) + u'.' +
           ext;
}

namespace {
QSet<QString> &disabledBucketSet() {
    static QSet<QString> s;
    return s;
}
} // namespace

void setDisabledBuckets(const QSet<QString> &disabled) {
    disabledBucketSet() = disabled;
}

bool bucketDisabled(BooruEngine engine, const QString &bucket) {
    if (bucket == QStringLiteral("general"))
        return false;
    return disabledBucketSet().contains(engineKey(engine) + u':' + bucket);
}

bool bucketDisabledFor(const QString &host, const QString &bucket) {
    if (bucket == QStringLiteral("general") || host.isEmpty())
        return false;
    return disabledBucketSet().contains(host.toLower() + u':' + bucket);
}

} // namespace RemotePosts

// Per-family singletons live in engine_danbooru.cpp and friends.
namespace EngineInternal {
const BooruEngineOps *danbooruOps();
const BooruEngineOps *gelbooruOps();
const BooruEngineOps *moebooruOps();
const BooruEngineOps *szurubooruOps();
const BooruEngineOps *shimmieOps();
const BooruEngineOps *hybooruOps();
const BooruEngineOps *nuuruOps();
const BooruEngineOps *derpibooruOps();
const BooruEngineOps *twibooruOps();
} // namespace EngineInternal

const BooruEngineOps *engineOps(BooruEngine engine) {
    using enum BooruEngine;
    switch (engine) {
    case Gelbooru:
    case Safebooru: // same dapi family
        return EngineInternal::gelbooruOps();
    case Moebooru:
        return EngineInternal::moebooruOps();
    case Szurubooru:
        return EngineInternal::szurubooruOps();
    case Shimmie:
        return EngineInternal::shimmieOps();
    case Hybooru:
        return EngineInternal::hybooruOps();
    case Nuuru:
        return EngineInternal::nuuruOps();
    case Derpibooru:
        return EngineInternal::derpibooruOps();
    case Twibooru:
        return EngineInternal::twibooruOps();
    case Danbooru:
        return EngineInternal::danbooruOps();
    }
    return EngineInternal::danbooruOps();
}
