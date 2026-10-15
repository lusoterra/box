#pragma once

// Disk-usage guard for the cache: thumbnails are tiny, but full-quality
// images are not, so the whole cache tree lives under a size cap (default
// 10 GB). Past the cap it rotates: least-recently-seen files are deleted
// first until back under budget.
//
// Tracking lives in a SQLite DB next to the cache itself (platform cache
// dir): it is bookkeeping and rebuilds from disk when lost. The cached
// files, plus the per-root index DBs, stay in the same tree.
//
// Threading: one persistent connection per thread (thread_local), all use
// serialized by a recursive mutex. Connections are NEVER removed at runtime -
// tearing down live connections is what crashed the app (Qt forbids
// removeDatabase while queries/handles exist).
#include <QMutex>
#include <QRecursiveMutex>
#include <QString>

class CacheJanitor {
public:
    static void configure(const QString &cacheBaseDir, qint64 maxBytes);
    // Where the tracking DB lives (default: effectiveDataDir()/cache.db).
    // Call before any other use when relocated.
    static void setTrackingDir(const QString &dir);
    static QString dbPath();
    // Record a newly written file (or re-record size changes).
    static void record(const QString &path);
    // Mark an existing file seen (cache hit). Adopts untracked files.
    static void hit(const QString &path);
    // Adopt unknown files, drop stale rows, delete oldest-seen past the cap.
    // Throttled: at most once a minute unless forced.
    static void enforce(bool force = false);

private:
    static QRecursiveMutex &mutex(); // recursive: hit() shares code with record()
    static QString &baseDir();
    static QString &trackingDir();
    static qint64 &maxBytes();
    static qint64 &lastCheckMs();
};
