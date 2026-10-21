#pragma once

// Settings backup/restore: one portable JSON file with the settings that
// travel (boorus incl. creds/cookies like settings.ini itself, fav tags,
// filters, theme, behaviour toggles) plus the favourites list. Machine
// paths (data/download/mascot dirs), window geometry and live session
// tabs stay out on purpose. Import merges: scalars/lists overwrite,
// boorus merge by key (local-only entries survive), favourites addMany
// (INSERT OR IGNORE skips duplicates, so re-imports are safe).
#include <QJsonObject>
#include <QString>
#include <QVector>

struct AppSettings;
struct Post;

namespace SettingsBackup {
QJsonObject exportJson(const AppSettings &s, const QVector<Post> &favs);
QJsonObject postToJson(const Post &p);
Post postFromJson(const QJsonObject &o);
// Merges into *s / appends to *favs. False + error on wrong shape.
bool importJson(const QJsonObject &o, AppSettings *s, QVector<Post> *favs,
                QString *error);
} // namespace SettingsBackup
