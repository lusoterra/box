#include "migrations.h"

// favicon-adopt: portable storage adopts auto-fetched site icons from the
// cache dir into <dataDir>/favicons/auto/ (user overrides in
// <dataDir>/favicons/ win and are never touched).
//
// FaviconCache::setCustomDir() calls applyFaviconAdopt() on every set
// (covers mid-session data-dir moves); the runner performs the same move
// at boot so it can report progress.
#include <QDir>
#include <QFile>

namespace Migrations {

bool needsFaviconAdopt(const QString &cacheDir, const QString &customDir) {
    if (customDir.isEmpty())
        return false;
    QDir cache(cacheDir);
    if (!cache.exists())
        return false;
    // Portable storage wins outright: once adopted, the cache dir is
    // empty (moves, not copies) so later boots never re-probe.
    const QStringList pngs =
        cache.entryList({QStringLiteral("*.png")}, QDir::Files);
    if (pngs.isEmpty())
        return false;
    QDir autoDir(customDir + QStringLiteral("/auto"));
    for (const QString &f : pngs) {
        if (!autoDir.exists(f))
            return true;
    }
    return false;
}

bool applyFaviconAdopt(const QString &cacheDir, const QString &customDir,
                       QString *error) {
    Q_UNUSED(error);
    if (customDir.isEmpty())
        return true;
    QDir().mkpath(customDir + QStringLiteral("/auto"));
    QDir cache(cacheDir);
    for (const QString &f :
         cache.entryList({QStringLiteral("*.png")}, QDir::Files)) {
        const QString key = f.left(f.size() - 4);
        if (key.isEmpty() || key.contains(u'/'))
            continue;
        const QString dest = customDir + QStringLiteral("/auto/") + f;
        if (QFile::exists(dest))
            continue;
        QFile::rename(cacheDir + u'/' + f, dest);
    }
    return true;
}

} // namespace Migrations
