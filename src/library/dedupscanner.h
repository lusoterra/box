#pragma once

// Background pixel-hash pass for duplicate detection. Enumerates one
// source family at a time (archive / download / hydrus), reuses cached
// rows whose size+mtime are unchanged, and prunes vanished files.
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <atomic>

#include "dedupdb.h"

struct DedupSource {
    QString scope; // archive | download | hydrus
    QString root;  // canonical directory
};

struct DedupScanOptions {
    QString dbPath;
    QString hydrusDbPath; // sha/width/height hints for the hydrus scope
    QString downloadDir;
    QString hydrusDir;
    QVector<DedupSource> sources;
};

class DedupScanTask : public QObject {
    Q_OBJECT
public:
    struct Result {
        int total = 0;
        int hashed = 0;
        int cached = 0;
        int failed = 0;
        bool cancelled = false;
        QString error;
    };

    explicit DedupScanTask(QObject *parent = nullptr) : QObject(parent) {}

    Result result;
    std::atomic<bool> cancelRequested{false};
    void requestCancel() { cancelRequested.store(true); }

    // Blocking; call via QtConcurrent::run. Emits progress from the worker.
    void run(const DedupScanOptions &options);

signals:
    void progress(int done, int total, const QString &scope,
                  const QString &path);

private:
    bool hashFile(const QString &path, const QString &scope,
                  const QString &root, const QString &shaHint,
                  int widthHint, int heightHint, DedupFile *out,
                  bool *cancelled);
};
