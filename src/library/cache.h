#pragma once

#include "post.h"

#include <QSet>
#include <QString>

// Persistent scan cache (SQLite, per watched root).
//
// Large archives rescan in seconds instead of minutes: each media file is
// keyed by (relative path, mtime, size). Unchanged files reuse the stored
// parsed metadata; only new/changed files hit their sidecar JSON.
// Entries for deleted files are pruned. Must be created, used and closed
// inside the worker thread that runs the scan (QSql connection affinity).
class ScanCache {
public:
    ScanCache() = default;
    ~ScanCache(); // rolls back an uncommitted batch (cancelled scans)

    // Opens (creating if needed) the cache DB for `rootAbs`. False on error
    // (caller then scans without cache). `explicitHash` identifies the
    // explicit-tag set: a mismatch wipes stored ratings so list edits apply.
    bool open(const QString &rootAbs, const QString &explicitHash);
    bool isOpen() const { return m_open; }

    // DB file path for a root (shared with TagTypeCache, other threads open
    // their own connection to the same file).
    static QString dbPathFor(const QString &rootAbs);

    bool lookup(const QString &relPath, qint64 mtime, qint64 size, Post &post);
    void store(const QString &relPath, qint64 mtime, qint64 size, const Post &post);

    // Drops rows for files not seen in this scan and commits.
    void finish(const QSet<QString> &seen);

    void close();

    // Cache statistics for the status bar.
    int hits() const { return m_hits; }
    int misses() const { return m_misses; }

private:
    bool m_open = false;
    QString m_conn;
    int m_hits = 0;
    int m_misses = 0;
    // Write batching: stores share one transaction (one fsync per scan,
    // not per file). Opened on first store, committed by finish().
    bool m_batch = false;
};
