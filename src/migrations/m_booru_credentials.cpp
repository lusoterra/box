#include "migrations.h"

// booru-credentials: first-run preset seeding, folding the legacy
// per-site credentials/bases (danbooru/user, gelbooru/key, …,
// szurubooru/base, shimmie/base) onto the matching presets, then dropping
// the legacy keys. Fresh profiles seed Safebooru-only (see presetBoorus);
// legacy credentials for unshipped boorus are dropped.
//
// loadBoorus() in src/settings.cpp keeps a fallback call to the same
// helper, so temp-config unit tests (which never run the runner) seed
// identically.
#include "settings.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSettings>

namespace Migrations {

namespace {
const QStringList kLegacyKeys = {
    QStringLiteral("danbooru/user"),  QStringLiteral("danbooru/key"),
    QStringLiteral("gelbooru/user"),  QStringLiteral("gelbooru/key"),
    QStringLiteral("yandere/user"),   QStringLiteral("yandere/pass"),
    QStringLiteral("konachan/user"),  QStringLiteral("konachan/pass"),
    QStringLiteral("szurubooru/base"), QStringLiteral("szurubooru/user"),
    QStringLiteral("szurubooru/token"), QStringLiteral("shimmie/base")};

bool legacyPresent(QSettings &c) {
    for (const QString &k : kLegacyKeys) {
        if (c.contains(k))
            return true;
    }
    return false;
}

QString jsonPath() {
    // Portable JSON wins when present: same lookup as loadBoorus().
    return QFileInfo(settingsFilePath()).absolutePath() +
           QStringLiteral("/boorus.json");
}
} // namespace

bool needsLegacyBooruCredentials(QSettings &c) {
    if (QFile::exists(jsonPath()))
        return false;
    if (!c.value(QStringLiteral("boorus/order")).toStringList().isEmpty())
        return false;
    // Registry missing AND legacy keys present: fold them onto the
    // presets. Pure first-run seeding (no legacy keys) stays lazy in
    // loadBoorus(); it is not migration work.
    return legacyPresent(c);
}

bool applyLegacyBooruCredentials(QSettings &c, QString *error) {
    Q_UNUSED(error);
    QList<BooruSite> out = RemoteSource::presetBoorus();
    auto migrateCreds = [&](const QString &key, const QString &userKey,
                            const QString &secretKey) {
        const QString user = c.value(userKey).toString();
        const QString secret = c.value(secretKey).toString();
        for (BooruSite &b : out) {
            if (b.key == key) {
                b.user = user;
                b.secret = secret;
            }
        }
    };
    migrateCreds(QStringLiteral("danbooru"), QStringLiteral("danbooru/user"),
                 QStringLiteral("danbooru/key"));
    migrateCreds(QStringLiteral("gelbooru"), QStringLiteral("gelbooru/user"),
                 QStringLiteral("gelbooru/key"));
    migrateCreds(QStringLiteral("yandere"), QStringLiteral("yandere/user"),
                 QStringLiteral("yandere/pass"));
    migrateCreds(QStringLiteral("konachan"), QStringLiteral("konachan/user"),
                 QStringLiteral("konachan/pass"));
    migrateCreds(QStringLiteral("szurubooru"), QStringLiteral("szurubooru/user"),
                 QStringLiteral("szurubooru/token"));
    for (BooruSite &b : out) {
        if (b.key == QStringLiteral("szurubooru"))
            b.baseUrl = c.value(QStringLiteral("szurubooru/base")).toString();
        else if (b.key == QStringLiteral("shimmie"))
            b.baseUrl = c.value(QStringLiteral("shimmie/base")).toString();
    }
    for (const QString &legacy : kLegacyKeys)
        c.remove(legacy);
    saveBoorus(c, out);
    c.sync();
    return true;
}

} // namespace Migrations
