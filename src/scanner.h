#pragma once

#include "post.h"

#include <QObject>
#include <QSet>
#include <QString>
#include <QVector>

// Recursive folder scan. Supports both sidecar layouts (read-only, never writes):
//   gallery-dl:  <mediafile>.json  (e.g. image.jpg.json, flat or native info.json)
//   LOSN:        <stem>.json        (e.g. Danbooru_<md5>.jpg + Danbooru_<md5>.json)
// Runs off the UI thread via QtConcurrent; emits progress as it goes.
// With useCache (default), parsed metadata persists in a per-root SQLite db
// keyed by (path, mtime, size) so rescans of large archives are incremental.
class Scanner : public QObject {
    Q_OBJECT
public:
    explicit Scanner(QObject *parent = nullptr) : QObject(parent) {}

    static const QStringList &mediaExtensions();

    // Blocking scan; call via QtConcurrent::run. Emits progress() on `owner`.
    // Tags that arrived untyped are added to `unknownOut` (if given) for
    // optional post-scan classification.
    static QVector<Post> scan(const QString &root, Scanner *owner = nullptr,
                              bool useCache = true,
                              QSet<QString> *unknownOut = nullptr);

signals:
    void progress(int filesFound, int totalFiles);
    void cacheStats(int hits, int misses);
};
