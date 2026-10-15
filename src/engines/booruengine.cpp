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
    if (v.isDouble())
        return static_cast<qint64>(v.toDouble());
    if (v.isString()) {
        bool ok = false;
        const qint64 n = v.toString().trimmed().toLongLong(&ok);
        return ok ? n : def;
    }
    return def;
}

QStringList BooruEngineOps::splitTags(const QString &s) {
    return s.split(QChar::Space, Qt::SkipEmptyParts);
}

QString BooruEngineOps::extOf(const QString &fileUrl, const QString &fallback) {
    const QString path = QUrl(fileUrl).path();
    const int dot = path.lastIndexOf(u'.');
    if (dot < 0)
        return fallback;
    const QString ext = path.mid(dot + 1).toLower();
    return ext.size() <= 5 ? ext : fallback;
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
                                  const QJsonObject &raw, BooruEngine engine) {
    p.isRemote = true;
    // Hygiene: every engine's tag blob funnels through here.
    p.tagsArtist = cleanTagTokens(splitTags(p.tagsArtist)).join(u' ');
    p.tagsCharacter = cleanTagTokens(splitTags(p.tagsCharacter)).join(u' ');
    p.tagsCopyright = cleanTagTokens(splitTags(p.tagsCopyright)).join(u' ');
    p.tagsGeneral = cleanTagTokens(splitTags(p.tagsGeneral)).join(u' ');
    p.tagsMeta = cleanTagTokens(splitTags(p.tagsMeta)).join(u' ');
    p.untyped = cleanTagTokens(splitTags(p.untyped)).join(u' ');
    // Disabled buckets for this booru type fold into general (non-anime
    // taxonomies). General itself never folds.
    auto foldBucket = [&](QString &field, const char *bucket) {
        if (!field.isEmpty() &&
            RemotePosts::bucketDisabled(engine, QString::fromLatin1(bucket))) {
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
    return BooruEngine::Danbooru;
}

QString engineLabel(BooruEngine engine) {
    switch (engine) {
    case BooruEngine::Danbooru: return QStringLiteral("Danbooru");
    case BooruEngine::Gelbooru: return QStringLiteral("Gelbooru");
    case BooruEngine::Moebooru: return QStringLiteral("Moebooru");
    case BooruEngine::Szurubooru: return QStringLiteral("Szurubooru");
    case BooruEngine::Shimmie: return QStringLiteral("Shimmie");
    case BooruEngine::Safebooru: return QStringLiteral("Safebooru");
    }
    return QStringLiteral("Danbooru");
}

QStringList translateSearchTags(const QStringList &tags, BooruEngine engine) {
    // "Safe" has no universal spelling: danbooru/moebooru/shimmie take
    // rating:safe, Gelbooru only understands rating:general, Szurubooru
    // wants safety:safe (safety:unsafe for explicit). Anything else passes
    // through untouched.
    QStringList out;
    out.reserve(tags.size());
    for (QString tok : tags) {
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
        out << tok;
    }
    return out;
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
    safe.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9]+")), QString());
    if (safe.isEmpty())
        safe = QStringLiteral("Booru");
    QString ext = raw.value(QStringLiteral("file_ext")).toString().toLower();
    if (ext.isEmpty()) {
        const QString url = raw.value(QStringLiteral("file_url")).toString();
        if (!url.isEmpty())
            ext = BooruEngineOps::extOf(url);
    }
    if (ext.isEmpty())
        ext = QStringLiteral("jpg");
    QString md5 = raw.value(QStringLiteral("md5")).toString();
    if (md5.isEmpty())
        md5 = raw.value(QStringLiteral("checksumMD5")).toString(); // szurubooru
    if (!md5.isEmpty())
        return safe + u'_' + md5 + u'.' + ext;
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

} // namespace RemotePosts

// Per-family singletons live in engine_danbooru.cpp and friends.
namespace EngineInternal {
const BooruEngineOps *danbooruOps();
const BooruEngineOps *gelbooruOps();
const BooruEngineOps *moebooruOps();
const BooruEngineOps *szurubooruOps();
const BooruEngineOps *shimmieOps();
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
    case BooruEngine::Danbooru:
        return EngineInternal::danbooruOps();
    }
    return EngineInternal::danbooruOps();
}
