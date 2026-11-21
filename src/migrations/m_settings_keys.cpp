#include "migrations.h"

// settings-keys: legacy/retired ini keys -> current keys.
//
// Covered (read fallbacks in loadSettings() stay: they cost nothing and
// keep tests that never run the runner working):
// - General/restoreSession -> startup/defaultTab ("true" becomes "last")
// - General/language -> app/language
// - cache/thumbs=false -> cache/mode=none (only when cache/mode absent)
// - retired keys removed: privacy/paranoia, search/history
#include <QSettings>

namespace Migrations {

bool needsSettingsKeys(QSettings &c) {
    if (c.contains(QStringLiteral("General/restoreSession")) ||
        c.contains(QStringLiteral("General/language")) ||
        c.contains(QStringLiteral("privacy/paranoia")) ||
        c.contains(QStringLiteral("search/history")))
        return true;
    return c.contains(QStringLiteral("cache/thumbs")) &&
           !c.contains(QStringLiteral("cache/mode"));
}

bool applySettingsKeys(QSettings &c, QString *error) {
    Q_UNUSED(error);
    // Legacy toggle: restoring the session meant reopening the last tab.
    if (c.contains(QStringLiteral("General/restoreSession")) &&
        !c.contains(QStringLiteral("startup/defaultTab")) &&
        c.value(QStringLiteral("General/restoreSession")).toBool())
        c.setValue(QStringLiteral("startup/defaultTab"),
                   QStringLiteral("last"));
    if (c.contains(QStringLiteral("General/language")) &&
        !c.contains(QStringLiteral("app/language")))
        c.setValue(QStringLiteral("app/language"),
                   c.value(QStringLiteral("General/language")).toString());
    // Legacy thumbnail disk-cache toggle: off meant no media cache.
    if (c.contains(QStringLiteral("cache/thumbs")) &&
        !c.contains(QStringLiteral("cache/mode")) &&
        !c.value(QStringLiteral("cache/thumbs"), true).toBool())
        c.setValue(QStringLiteral("cache/mode"), QStringLiteral("none"));
    c.remove(QStringLiteral("General/restoreSession"));
    c.remove(QStringLiteral("General/language"));
    c.remove(QStringLiteral("privacy/paranoia")); // retired: false promise
    c.remove(QStringLiteral("search/history"));   // retired: history removed
    c.sync();
    return true;
}

} // namespace Migrations
