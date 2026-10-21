#include "settingsexport.h"

#include "settings.h"
#include "tagtypes.h"

#include <QJsonArray>
#include <QObject>

namespace SettingsBackup {
namespace {
QJsonArray strList(const QStringList &in) {
    QJsonArray a;
    for (const QString &s : in)
        a.append(s);
    return a;
}
QStringList strList(const QJsonValue &v) {
    QStringList out;
    for (const QJsonValue &e : v.toArray()) {
        if (e.isString())
            out.append(e.toString());
    }
    return out;
}
QJsonObject booruToJson(const BooruSite &b) {
    QJsonObject o;
    o[QStringLiteral("key")] = b.key;
    o[QStringLiteral("label")] = b.label;
    o[QStringLiteral("short")] = b.shortName;
    o[QStringLiteral("badgeColor")] = b.badgeColor;
    o[QStringLiteral("baseUrl")] = b.baseUrl;
    o[QStringLiteral("engine")] = RemotePosts::engineKey(b.engine);
    o[QStringLiteral("needsCreds")] = b.needsCreds;
    o[QStringLiteral("user")] = b.user;
    o[QStringLiteral("secret")] = b.secret;
    o[QStringLiteral("cookies")] = strList(b.cookies);
    o[QStringLiteral("skipAutoClassify")] = b.skipAutoClassify;
    QJsonArray dis;
    for (const QString &d : b.disabledBuckets)
        dis.append(d);
    o[QStringLiteral("disabledBuckets")] = dis;
    o[QStringLiteral("defaultTags")] = b.defaultTags;
    o[QStringLiteral("proxy")] = b.proxy;
    return o;
}
BooruSite booruFromJson(const QJsonObject &o) {
    BooruSite b;
    b.key = o.value(QStringLiteral("key")).toString();
    b.label = o.value(QStringLiteral("label")).toString();
    b.shortName = o.value(QStringLiteral("short")).toString();
    if (!o.value(QStringLiteral("badgeColor")).toString().isEmpty())
        b.badgeColor = o.value(QStringLiteral("badgeColor")).toString();
    b.baseUrl = o.value(QStringLiteral("baseUrl")).toString();
    b.engine = RemotePosts::engineForKey(
        o.value(QStringLiteral("engine")).toString());
    b.needsCreds = o.value(QStringLiteral("needsCreds")).toBool();
    b.user = o.value(QStringLiteral("user")).toString();
    b.secret = o.value(QStringLiteral("secret")).toString();
    b.cookies = strList(o.value(QStringLiteral("cookies")));
    b.skipAutoClassify = o.value(QStringLiteral("skipAutoClassify")).toBool();
    for (const QJsonValue &d :
         o.value(QStringLiteral("disabledBuckets")).toArray()) {
        if (d.isString() && !d.toString().isEmpty())
            b.disabledBuckets.insert(d.toString());
    }
    b.defaultTags = o.value(QStringLiteral("defaultTags")).toString();
    b.proxy = o.value(QStringLiteral("proxy")).toString();
    return b;
}
} // namespace

QJsonObject postToJson(const Post &p) {
    QJsonObject o;
    o[QStringLiteral("site")] = p.siteName;
    o[QStringLiteral("postId")] = QString::number(p.id);
    o[QStringLiteral("rating")] = p.rating;
    o[QStringLiteral("score")] = p.score;
    o[QStringLiteral("ext")] = p.ext;
    o[QStringLiteral("tags")] = p.tags;
    o[QStringLiteral("tagsArtist")] = p.tagsArtist;
    o[QStringLiteral("tagsCharacter")] = p.tagsCharacter;
    o[QStringLiteral("tagsCopyright")] = p.tagsCopyright;
    o[QStringLiteral("tagsGeneral")] = p.tagsGeneral;
    o[QStringLiteral("tagsMeta")] = p.tagsMeta;
    o[QStringLiteral("fileUrl")] = p.fileUrl;
    o[QStringLiteral("sampleUrl")] = p.sampleUrl;
    o[QStringLiteral("previewUrl")] = p.previewUrl;
    o[QStringLiteral("postUrl")] = p.postUrl;
    o[QStringLiteral("width")] = p.width;
    o[QStringLiteral("height")] = p.height;
    o[QStringLiteral("untyped")] = p.untyped;
    QJsonObject custom;
    for (auto it = p.customTags.begin(); it != p.customTags.end(); ++it) {
        QJsonArray arr;
        for (const QString &t : it.value())
            arr.append(t);
        custom.insert(it.key(), arr);
    }
    o[QStringLiteral("customTags")] = custom;
    return o;
}

Post postFromJson(const QJsonObject &o) {
    Post p;
    p.isRemote = true;
    p.siteName = o.value(QStringLiteral("site")).toString();
    p.id = o.value(QStringLiteral("postId")).toString().toLongLong();
    p.rating = o.value(QStringLiteral("rating")).toString();
    if (p.rating.isEmpty())
        p.rating = QStringLiteral("general");
    p.score = o.value(QStringLiteral("score")).toInt();
    p.ext = o.value(QStringLiteral("ext")).toString();
    p.tags = o.value(QStringLiteral("tags")).toString();
    p.tagsArtist = o.value(QStringLiteral("tagsArtist")).toString();
    p.tagsCharacter = o.value(QStringLiteral("tagsCharacter")).toString();
    p.tagsCopyright = o.value(QStringLiteral("tagsCopyright")).toString();
    p.tagsGeneral = o.value(QStringLiteral("tagsGeneral")).toString();
    p.tagsMeta = o.value(QStringLiteral("tagsMeta")).toString();
    p.fileUrl = o.value(QStringLiteral("fileUrl")).toString();
    p.sampleUrl = o.value(QStringLiteral("sampleUrl")).toString();
    p.previewUrl = o.value(QStringLiteral("previewUrl")).toString();
    p.postUrl = o.value(QStringLiteral("postUrl")).toString();
    p.width = o.value(QStringLiteral("width")).toInt();
    p.height = o.value(QStringLiteral("height")).toInt();
    p.untyped = o.value(QStringLiteral("untyped")).toString();
    const QJsonObject custom = o.value(QStringLiteral("customTags")).toObject();
    for (auto it = custom.begin(); it != custom.end(); ++it) {
        QStringList tags;
        for (const QJsonValue &t : it.value().toArray()) {
            if (t.isString())
                tags.append(t.toString());
        }
        if (!tags.isEmpty())
            p.customTags.insert(it.key(), tags);
    }
    p.hasMeta = true;
    return p;
}

QJsonObject exportJson(const AppSettings &s, const QVector<Post> &favs) {
    QJsonObject o;
    o[QStringLiteral("app")] = QStringLiteral("box");
    o[QStringLiteral("format")] = 1;
    QJsonObject browse;
    browse[QStringLiteral("mode")] = s.browseMode;
    browse[QStringLiteral("sites")] = strList(s.browseSites);
    browse[QStringLiteral("limit")] = s.browseLimit;
    browse[QStringLiteral("wallAutoPopup")] = s.wallAutoPopup;
    o[QStringLiteral("browse")] = browse;
    QJsonObject fav;
    fav[QStringLiteral("autoDownload")] = s.favAutoDownload;
    fav[QStringLiteral("autoFav")] = s.favAutoFav;
    fav[QStringLiteral("unfavDeletesFile")] = s.unfavDeletesFile;
    fav[QStringLiteral("deleteUnfavs")] = s.deleteUnfavs;
    fav[QStringLiteral("tags")] = strList(s.favTags);
    o[QStringLiteral("fav")] = fav;
    QJsonObject filter;
    filter[QStringLiteral("blacklist")] = strList(s.blacklist);
    filter[QStringLiteral("hideAi")] = s.hideAi;
    o[QStringLiteral("filter")] = filter;
    QJsonArray boorus;
    for (const BooruSite &b : s.boorus) {
        if (b.key.trimmed().isEmpty())
            continue;
        boorus.append(booruToJson(b));
    }
    o[QStringLiteral("boorus")] = boorus;
    QJsonArray dis;
    for (const QString &d : s.disabledBuckets)
        dis.append(d);
    o[QStringLiteral("disabledBuckets")] = dis;
    QJsonObject manual;
    for (auto it = s.manualTagTypes.begin(); it != s.manualTagTypes.end();
         ++it)
        manual.insert(it.key(), it.value());
    o[QStringLiteral("manualTagTypes")] = manual;
    QJsonObject customBuckets;
    for (auto it = s.customBucketMap.begin(); it != s.customBucketMap.end();
         ++it)
        customBuckets.insert(it.key(), it.value());
    o[QStringLiteral("customBuckets")] = customBuckets;
    o[QStringLiteral("explicitTags")] = strList(s.explicitTags);
    QJsonObject theme;
    theme[QStringLiteral("mode")] = s.themeMode;
    theme[QStringLiteral("accent")] = s.accent;
    o[QStringLiteral("theme")] = theme;
    QJsonObject ugoira;
    ugoira[QStringLiteral("format")] = s.ugoiraFormat;
    ugoira[QStringLiteral("frameDelayMs")] = s.ugoiraFrameDelayMs;
    o[QStringLiteral("ugoira")] = ugoira;
    o[QStringLiteral("tabsDefault")] = s.tabsDefault;
    o[QStringLiteral("language")] = s.language;
    QJsonObject video;
    video[QStringLiteral("muted")] = s.videoMuted;
    video[QStringLiteral("loop")] = s.videoLoop;
    video[QStringLiteral("volume")] = s.volume;
    o[QStringLiteral("video")] = video;
    o[QStringLiteral("viewerMaxSide")] = s.viewerMaxSide;
    QJsonObject grid;
    grid[QStringLiteral("aspectNum")] = s.aspectNum;
    grid[QStringLiteral("aspectDen")] = s.aspectDen;
    grid[QStringLiteral("boxW")] = s.gridBoxW;
    grid[QStringLiteral("scrollRows")] = s.gridScrollRows;
    o[QStringLiteral("grid")] = grid;
    QJsonArray favList;
    for (const Post &p : favs)
        favList.append(postToJson(p));
    o[QStringLiteral("favourites")] = favList;
    return o;
}

bool importJson(const QJsonObject &o, AppSettings *s, QVector<Post> *favs,
                QString *error) {
    auto fail = [&](const QString &msg) {
        if (error)
            *error = msg;
        return false;
    };
    if (o.value(QStringLiteral("app")).toString() != QStringLiteral("box"))
        return fail(QObject::tr("Not a box settings backup"));
    if (o.value(QStringLiteral("format")).toInt(-1) != 1)
        return fail(QObject::tr("Unsupported backup format"));
    if (!s || !favs)
        return fail(QObject::tr("Internal error"));
    const QJsonObject browse = o.value(QStringLiteral("browse")).toObject();
    if (!browse.value(QStringLiteral("mode")).toString().isEmpty())
        s->browseMode = browse.value(QStringLiteral("mode")).toString();
    if (browse.contains(QStringLiteral("sites")))
        s->browseSites = strList(browse.value(QStringLiteral("sites")));
    if (browse.contains(QStringLiteral("limit")))
        s->browseLimit =
            qBound(10, browse.value(QStringLiteral("limit")).toInt(40), 200);
    if (browse.contains(QStringLiteral("wallAutoPopup")))
        s->wallAutoPopup = browse.value(QStringLiteral("wallAutoPopup")).toBool(true);
    const QJsonObject fav = o.value(QStringLiteral("fav")).toObject();
    if (fav.contains(QStringLiteral("autoDownload")))
        s->favAutoDownload = fav.value(QStringLiteral("autoDownload")).toBool();
    if (fav.contains(QStringLiteral("autoFav")))
        s->favAutoFav = fav.value(QStringLiteral("autoFav")).toBool();
    if (fav.contains(QStringLiteral("unfavDeletesFile")))
        s->unfavDeletesFile = fav.value(QStringLiteral("unfavDeletesFile")).toBool();
    if (fav.contains(QStringLiteral("deleteUnfavs")))
        s->deleteUnfavs = fav.value(QStringLiteral("deleteUnfavs")).toBool();
    if (fav.contains(QStringLiteral("tags")))
        s->favTags = strList(fav.value(QStringLiteral("tags")));
    const QJsonObject filter = o.value(QStringLiteral("filter")).toObject();
    if (filter.contains(QStringLiteral("blacklist")))
        s->blacklist = normalizeExplicitTags(
            strList(filter.value(QStringLiteral("blacklist"))));
    if (filter.contains(QStringLiteral("hideAi")))
        s->hideAi = filter.value(QStringLiteral("hideAi")).toBool();
    if (o.contains(QStringLiteral("boorus"))) {
        for (const QJsonValue &v : o.value(QStringLiteral("boorus")).toArray()) {
            if (!v.isObject())
                continue;
            const BooruSite b = booruFromJson(v.toObject());
            if (b.key.trimmed().isEmpty())
                continue;
            bool replaced = false;
            for (BooruSite &live : s->boorus) {
                if (live.key == b.key) {
                    live = b;
                    replaced = true;
                    break;
                }
            }
            if (!replaced)
                s->boorus.append(b);
        }
    }
    if (o.contains(QStringLiteral("disabledBuckets"))) {
        s->disabledBuckets.clear();
        for (const QString &d : strList(o.value(QStringLiteral("disabledBuckets")))) {
            if (!d.trimmed().isEmpty())
                s->disabledBuckets.insert(d.trimmed().toLower());
        }
    }
    if (o.contains(QStringLiteral("manualTagTypes"))) {
        QStringList raw;
        const QJsonObject manual = o.value(QStringLiteral("manualTagTypes")).toObject();
        for (auto it = manual.begin(); it != manual.end(); ++it) {
            if (it.value().isString())
                raw.append(it.key() + QStringLiteral("=") + it.value().toString());
        }
        s->manualTagTypes = TagTypes::normalizeTagOverrides(raw);
    }
    if (o.contains(QStringLiteral("customBuckets"))) {
        QStringList raw;
        const QJsonObject buckets = o.value(QStringLiteral("customBuckets")).toObject();
        for (auto it = buckets.begin(); it != buckets.end(); ++it) {
            if (it.value().isString())
                raw.append(it.key() + QStringLiteral("=") + it.value().toString());
        }
        s->customBucketMap = TagTypes::normalizeTagOverrides(raw);
    }
    if (o.contains(QStringLiteral("explicitTags")))
        s->explicitTags =
            normalizeExplicitTags(strList(o.value(QStringLiteral("explicitTags"))));
    const QJsonObject theme = o.value(QStringLiteral("theme")).toObject();
    if (!theme.value(QStringLiteral("mode")).toString().isEmpty())
        s->themeMode = theme.value(QStringLiteral("mode")).toString();
    if (!theme.value(QStringLiteral("accent")).toString().isEmpty())
        s->accent = theme.value(QStringLiteral("accent")).toString();
    const QJsonObject ugoira = o.value(QStringLiteral("ugoira")).toObject();
    const QString uf = ugoira.value(QStringLiteral("format")).toString();
    if (uf == QStringLiteral("webp") || uf == QStringLiteral("jxl") ||
        uf == QStringLiteral("zip"))
        s->ugoiraFormat = uf;
    if (ugoira.contains(QStringLiteral("frameDelayMs")))
        s->ugoiraFrameDelayMs = qBound(
            20, ugoira.value(QStringLiteral("frameDelayMs")).toInt(100), 2000);
    if (!o.value(QStringLiteral("tabsDefault")).toString().isEmpty())
        s->tabsDefault = o.value(QStringLiteral("tabsDefault")).toString();
    if (o.contains(QStringLiteral("language")))
        s->language = o.value(QStringLiteral("language")).toString();
    const QJsonObject video = o.value(QStringLiteral("video")).toObject();
    if (video.contains(QStringLiteral("muted")))
        s->videoMuted = video.value(QStringLiteral("muted")).toBool();
    if (video.contains(QStringLiteral("loop")))
        s->videoLoop = video.value(QStringLiteral("loop")).toBool();
    if (video.contains(QStringLiteral("volume")))
        s->volume = qBound(0, video.value(QStringLiteral("volume")).toInt(100), 100);
    if (o.contains(QStringLiteral("viewerMaxSide")))
        s->viewerMaxSide =
            qBound(512, o.value(QStringLiteral("viewerMaxSide")).toInt(2560), 8192);
    const QJsonObject grid = o.value(QStringLiteral("grid")).toObject();
    if (grid.contains(QStringLiteral("aspectNum")) &&
        grid.value(QStringLiteral("aspectNum")).toInt() > 0)
        s->aspectNum = grid.value(QStringLiteral("aspectNum")).toInt();
    if (grid.contains(QStringLiteral("aspectDen")) &&
        grid.value(QStringLiteral("aspectDen")).toInt() > 0)
        s->aspectDen = grid.value(QStringLiteral("aspectDen")).toInt();
    if (grid.contains(QStringLiteral("boxW")))
        s->gridBoxW =
            qBound(96, grid.value(QStringLiteral("boxW")).toInt(184), 384);
    if (grid.contains(QStringLiteral("scrollRows")))
        s->gridScrollRows = qBound(
            0.1, grid.value(QStringLiteral("scrollRows")).toDouble(0.5), 3.0);
    favs->clear();
    for (const QJsonValue &v : o.value(QStringLiteral("favourites")).toArray()) {
        if (!v.isObject())
            continue;
        const Post p = postFromJson(v.toObject());
        if (!p.siteName.isEmpty() || p.id != 0)
            favs->append(p);
    }
    return true;
}
} // namespace SettingsBackup
