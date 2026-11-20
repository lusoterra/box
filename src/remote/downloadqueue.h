#pragma once

// Crash-safe download queue journal (SQLite, one row per unfinished job).
//
// The DownloadManager writes every accepted job here before it starts and
// removes it when the job lands (or is explicitly dropped). A quit, crash
// or power loss therefore leaves exactly the unfinished work behind, and
// the next launch can offer to continue it instead of silently losing a
// large batch.
//
// Only queued/in-flight jobs live in the table: completed, skipped and
// failed jobs are deleted immediately. Partial bytes are written to a
// `<dest>.part` sibling and renamed on completion, so a stale row can
// never be mistaken for a finished file.
#include <QJsonObject>
#include <QList>
#include <QString>

#include "post.h"

struct DownloadJob;

class DownloadQueueStore {
public:
    explicit DownloadQueueStore(const QString &dbPath = QString());
    ~DownloadQueueStore();

    DownloadQueueStore(const DownloadQueueStore &) = delete;
    DownloadQueueStore &operator=(const DownloadQueueStore &) = delete;

    bool isOpen() const { return !m_conn.isEmpty(); }
    QString path() const { return m_path; }

    // Insert/replace by file URL. All jobs in one transaction.
    bool add(const QList<DownloadJob> &jobs);
    bool removeUrl(const QString &url);
    bool clear();
    QList<DownloadJob> load() const;
    int count() const;

    // Full Post + DownloadJob round-trip used by the journal. Pure, so
    // tests can verify field coverage without touching SQLite.
    static QJsonObject jobToJson(const DownloadJob &job);
    static bool jobFromJson(const QJsonObject &obj, DownloadJob &job);

private:
    void close();
    bool open();

    QString m_path;
    QString m_conn;
};
