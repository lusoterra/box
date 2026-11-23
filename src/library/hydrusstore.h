#pragma once

// Hydrus tab storage. This is deliberately its own little world:
// `hydrus.db` + a hydrus media folder, keyed by SHA-256. Nothing here
// reads or writes the booru favourites DB, downloaded files, the scan
// index or the tag-type cache. The only foreign data the tab may read
// is the booru tag-count cache (read-only) for the spam filter.
//
// Tags are stored in their native Hydrus namespaces (`creator:`,
// `character:`, `series:`, `species:`, `rating:`, ...). The namespace
// prefix is preserved in the DB/search tokens; the viewer/sidebar strip
// it for display and group by namespace. Unnamespaced tags are General.
//
// Hydrus treats tags case-insensitively; the tab stores them lowercase
// with whitespace folded to underscores so they fit the app's
// space-separated search model.
#include "post.h"

#include <QHash>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QVector>

namespace HydrusStore {

// One normalized tag split at the first ':' ("creator:alice").
struct SplitTag {
    QString ns;  // lowercase namespace; empty for general
    QString tag; // value without namespace
    bool namespaced() const { return !ns.isEmpty(); }
};
SplitTag splitTag(const QString &tag);

// Normalize one Hydrus tag: trim, lowercase, fold whitespace to '_',
// split at the first ':' (namespace kept lowercase). Empty -> "".
QString normalizeTag(const QString &raw);
// Normalize + dedupe (case-insensitive) + sort by namespace then tag.
QStringList normalizeTags(const QStringList &raw);

QMap<QString, QStringList> groupByNamespace(const QStringList &tags);

// `<sha256>.<ext>` (no dot when ext is empty). Lowercase hash/ext.
QString fileNameForHash(const QString &sha256, const QString &ext);

bool isSha256Hex(const QString &s);

// Read-only spam filter over the booru tag-count cache. Candidate keys
// are the full tag and the namespace-stripped value (both lowercase).
// Unknown tags are kept unless `dropUnknown` is set; 0 disables the
// filter entirely.
struct SpamFilter {
    int minPosts = 10;
    bool dropUnknown = false;
};
QStringList applySpamFilter(const QStringList &tags,
                            const QHash<QString, int> &cachedCounts,
                            const SpamFilter &filter);

// One staged source file after hashing/copying (worker result).
struct StagedFile {
    QString sha256;
    QString ext;
    QString source; // original path
    QString target; // final path in the hydrus media folder
    QString error;
    bool duplicate = false; // hash already imported (or target exists)
};

// One hydrus.db row.
struct Record {
    QString sha256;
    QString fileName; // actual file name in the media folder
    QString ext;
    qint64 bytes = 0;
    int width = 0;
    int height = 0;
    qint64 addedMs = 0;
    QString source; // pasted gallery page / original path, may be empty
    qint64 hydrusFileId = 0;
    bool found = false;          // last lookup found the hash
    bool manualOverride = false; // tags were edited by hand
    qint64 lastLookupMs = 0;
    QStringList tags; // canonical namespaced tags

    // Build the grid/viewer row. The Post search tokens carry both the
    // namespaced tag and its stripped value, so typing either one
    // matches; customTags holds the stripped values by namespace for the
    // native sidebar/viewer groups.
    Post toPost(const QString &mediaDir) const;
};

// SQLite store (one connection per instance; never shared across threads).
class Db {
public:
    Db() = default;
    ~Db() { close(); }
    Db(const Db &) = delete;
    Db &operator=(const Db &) = delete;

    bool open(const QString &dbPath);
    bool isOpen() const { return m_open; }
    void close();
    QString path() const { return m_path; }

    int count() const;
    bool contains(const QString &sha256) const;
    Record record(const QString &sha256) const;
    // Newest first.
    QVector<Record> list() const;

    // Insert when absent (duplicate hash -> false, existing row untouched).
    bool insert(const Record &r);
    // Replace the tag set and lookup outcome. clearOverride resets the
    // manual-edit protection (a forced refresh the user accepted).
    bool updateTags(const QString &sha256, const QStringList &tags, bool found,
                    qint64 fileId, qint64 lookupMs,
                    bool clearOverride = false);
    // Manual edit: tags replace the set and mark the row overridden.
    bool updateManualTags(const QString &sha256, const QStringList &tags);
    // A lookup that found nothing (or failed): keep the old tags, record
    // the miss so "Not on Hydrus" can find the row again.
    bool markMissed(const QString &sha256);
    // Orphan file discovered in the media folder: bind a real name/ext.
    bool updateFile(const QString &sha256, const QString &fileName,
                    const QString &ext, qint64 bytes);

private:
    bool exec(const QString &sql) const;
    bool m_open = false;
    QString m_conn;
    QString m_path;
};

} // namespace HydrusStore
