#pragma once

// Persistent pixel-hash cache for duplicate detection (`dedup.db`).
//
// The database is derived data: every row can be rebuilt from the files,
// so deleting it is always safe. Rows are keyed by (scope, path); the
// saved size+mtime make rescans incremental. Scopes are deliberately
// isolated duplicate families:
//   archive  - local archive roots (all roots share one family)
//   download - the download folder (downloaded favourites live here)
//   hydrus   - the Hydrus tab's managed media folder
// Groups are only ever built inside one (scope, kind) pair, so Hydrus,
// booru downloads and archives never match each other.
#include <QSet>
#include <QString>
#include <QVector>

struct DedupFile {
    qint64 id = 0;
    QString scope;
    QString root; // canonical directory the file was scanned from
    QString path; // canonical absolute media path
    qint64 size = 0;
    qint64 mtime = 0;
    QString kind;  // image | animated | video
    QString sha256; // exact-bytes identity (lowercase hex)
    quint64 phash = 0;
    bool hasPhash = false;
    int width = 0;
    int height = 0;
    qint64 hashedMs = 0;
};

class DedupDb {
public:
    DedupDb() = default;
    ~DedupDb();
    DedupDb(const DedupDb &) = delete;
    DedupDb &operator=(const DedupDb &) = delete;

    bool open(const QString &dbPath);
    bool isOpen() const { return m_open; }
    QString path() const { return m_path; }
    void close();

    // All rows, or only one scope. Ordered by id for stable group creation.
    QVector<DedupFile> files(const QString &scope = {}) const;

    // Write batching: the scanner calls begin(), then commit() every N
    // rows; rollback() discards a cancelled batch.
    bool begin();
    bool commit();
    bool rollback();
    bool upsert(const DedupFile &file);

    bool removePath(const QString &scope, const QString &path);
    bool removeId(qint64 id);
    // Drops rows for a (scope, root) that were not seen in this scan.
    void prune(const QString &scope, const QString &root,
               const QSet<QString> &seen);
    bool clear();
    int count() const;

    QString lastError() const { return m_error; }

private:
    bool exec(const QString &sql) const;
    bool m_open = false;
    bool m_transaction = false;
    QString m_conn;
    QString m_path;
    mutable QString m_error;
};
