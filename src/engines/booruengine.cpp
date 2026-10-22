// BooruEngineOps base: quirk-aware API root, shared mapping helpers,
// engine taxonomy and other pure helpers. Per-family request/response
// shapes live in engine_danbooru.cpp and friends.
#include "booruengine.h"
#include "explicittags.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QTimeZone>
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

void BooruEngineOps::finishRemote(Post &p, const QString &siteLabel,
                                  const QJsonObject &raw, BooruEngine engine,
                                  const QString &host) {
    p.isRemote = true;
    // Hygiene: every engine's tag blob funnels through here.
    p.tagsArtist = cleanTagTokens(splitTags(p.tagsArtist)).join(u' ');
    p.tagsCharacter = cleanTagTokens(splitTags(p.tagsCharacter)).join(u' ');
    p.tagsCopyright = cleanTagTokens(splitTags(p.tagsCopyright)).join(u' ');
    p.tagsGeneral = cleanTagTokens(splitTags(p.tagsGeneral)).join(u' ');
    p.tagsMeta = cleanTagTokens(splitTags(p.tagsMeta)).join(u' ');
    p.untyped = cleanTagTokens(splitTags(p.untyped)).join(u' ');
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
    switch (engine) {
    case BooruEngine::Danbooru: return QStringLiteral("danbooru");
    case BooruEngine::Gelbooru: return QStringLiteral("gelbooru");
    case BooruEngine::Moebooru: return QStringLiteral("moebooru");
    case BooruEngine::Szurubooru: return QStringLiteral("szurubooru");
    case BooruEngine::Shimmie: return QStringLiteral("shimmie");
    case BooruEngine::Hybooru: return QStringLiteral("hybooru");
    case BooruEngine::Nuuru: return QStringLiteral("nuuru");
    case BooruEngine::Hiddenbooru: return QStringLiteral("hiddenbooru");
    case BooruEngine::Safebooru: return QStringLiteral("gelbooru"); // dapi family
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
    if (key == QStringLiteral("hiddenbooru"))
        return BooruEngine::Hiddenbooru;
    return BooruEngine::Danbooru;
}

QString engineLabel(BooruEngine engine) {
    switch (engine) {
    case BooruEngine::Danbooru: return QStringLiteral("Danbooru");
    case BooruEngine::Gelbooru: return QStringLiteral("Gelbooru");
    case BooruEngine::Moebooru: return QStringLiteral("Moebooru");
    case BooruEngine::Szurubooru: return QStringLiteral("Szurubooru");
    case BooruEngine::Shimmie: return QStringLiteral("Shimmie");
    case BooruEngine::Hybooru: return QStringLiteral("HyBooru");
    case BooruEngine::Nuuru: return QStringLiteral("Nuuru (Soybooru)");
    case BooruEngine::Hiddenbooru: return QStringLiteral("HiddenBooru");
    case BooruEngine::Safebooru: return QStringLiteral("Safebooru");
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
    // words first so the rules below apply to them too — otherwise a
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
                   engine == BooruEngine::Hiddenbooru) {
            // Neither engine exposes a verified content-rating or sort
            // spelling (HyBooru rating: is a 0-100 star filter): such
            // tokens would silently mis-filter, so drop them — sorting
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
            // empty), so drop it — the client-side net still filters.
            continue;
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
        if (!dropped.contains(tok))
            tags << tok;
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
    if (siteKey == QStringLiteral("danbooru"))
        return baseUrl + QStringLiteral("/posts/") + QString::number(id);
    if (siteKey == QStringLiteral("yandere") || siteKey == QStringLiteral("konachan"))
        return baseUrl + QStringLiteral("/post/show/") + QString::number(id);
    if (siteKey == QStringLiteral("szurubooru"))
        return baseUrl + QStringLiteral("/post/") + QString::number(id);
    if (siteKey == QStringLiteral("shimmie"))
        return baseUrl + QStringLiteral("/post/view/") + QString::number(id);
    return baseUrl + QStringLiteral("/index.php?page=post&s=view&id=") +
           QString::number(id);
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
const BooruEngineOps *hiddenbooruOps();
} // namespace EngineInternal

const BooruEngineOps *engineOps(BooruEngine engine) {
    switch (engine) {
    case BooruEngine::Gelbooru:
    case BooruEngine::Safebooru: // same dapi family
        return EngineInternal::gelbooruOps();
    case BooruEngine::Moebooru:
        return EngineInternal::moebooruOps();
    case BooruEngine::Szurubooru:
        return EngineInternal::szurubooruOps();
    case BooruEngine::Shimmie:
        return EngineInternal::shimmieOps();
    case BooruEngine::Hybooru:
        return EngineInternal::hybooruOps();
    case BooruEngine::Nuuru:
        return EngineInternal::nuuruOps();
    case BooruEngine::Hiddenbooru:
        return EngineInternal::hiddenbooruOps();
    case BooruEngine::Danbooru:
        return EngineInternal::danbooruOps();
    }
    return EngineInternal::danbooruOps();
}
