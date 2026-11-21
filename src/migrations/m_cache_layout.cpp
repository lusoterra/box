#include "migrations.h"

// cache-layout: the pre-rename on-disk layout nested the cache twice
// (~/.cache/booru-explorer/booru-explorer/{thumbs,roots,favicons}) and
// kept cache.db in the data dir. Carry both over to the platform cache
// dir when the new spots are still empty.
//
// MainWindow keeps calling applyCacheLayout() on every boot (a few
// QFile::exists probes, no-ops once moved); the runner performs the same
// move up front so it can report progress.
#include <QDir>
#include <QFile>
#include <QStandardPaths>

namespace Migrations {

bool needsCacheLayout(const QString &cacheBase, const QString &dataDir) {
    const QString oldBase =
        QStandardPaths::writableLocation(QStandardPaths::CacheLocation) +
        QStringLiteral("/booru-explorer");
    for (const QString &sub :
         {QStringLiteral("thumbs"), QStringLiteral("roots"),
          QStringLiteral("favicons")}) {
        const QString dst = cacheBase + u'/' + sub;
        const QString src = oldBase + QStringLiteral("/booru-explorer/") + sub;
        if (!QDir(dst).exists() && QDir(src).exists())
            return true;
    }
    const QString newDb = cacheBase + QStringLiteral("/cache.db");
    const QString oldDb = dataDir + QStringLiteral("/cache.db");
    return !QFile::exists(newDb) && QFile::exists(oldDb);
}

bool applyCacheLayout(const QString &cacheBase, const QString &dataDir,
                      QString *error) {
    Q_UNUSED(error);
    const QString oldBase =
        QStandardPaths::writableLocation(QStandardPaths::CacheLocation) +
        QStringLiteral("/booru-explorer");
    for (const QString &sub :
         {QStringLiteral("thumbs"), QStringLiteral("roots"),
          QStringLiteral("favicons")}) {
        const QString dst = cacheBase + u'/' + sub;
        const QString src = oldBase + QStringLiteral("/booru-explorer/") + sub;
        if (!QDir(dst).exists() && QDir(src).exists()) {
            QDir().mkpath(cacheBase);
            QDir().rename(src, dst);
        }
    }
    const QString newDb = cacheBase + QStringLiteral("/cache.db");
    const QString oldDb = dataDir + QStringLiteral("/cache.db");
    if (!QFile::exists(newDb) && QFile::exists(oldDb)) {
        QDir().mkpath(cacheBase);
        if (!QFile::rename(oldDb, newDb) && QFile::copy(oldDb, newDb))
            QFile::remove(oldDb); // cross-device fallback (tmpfs cache)
    }
    return true;
}

} // namespace Migrations
