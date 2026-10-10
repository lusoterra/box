#include "scanner.h"
#include "cache.h"
#include "explicittags.h"
#include "metaparser.h"

#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

const QStringList &Scanner::mediaExtensions() {
    static const QStringList exts = {
        QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("jpeg"),
        QStringLiteral("gif"), QStringLiteral("webp"), QStringLiteral("avif"),
        QStringLiteral("mp4"), QStringLiteral("webm"),
    };
    return exts;
}

namespace {

bool loadSidecar(const QString &path, Post &post) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return false;
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
        return false;
    return MetaParser::parse(doc.object(), post);
}

QString categoryFor(const QStringList &dirParts, const QString &fileName) {
    if (!dirParts.isEmpty())
        return dirParts.first();
    // Flat folder (like testfiles/): Danbooru_<md5>.jpg -> Danbooru
    const QString stem = fileName.left(fileName.lastIndexOf(u'.'));
    const int us = stem.indexOf(u'_');
    if (us > 0)
        return stem.split(u'_').value(0);
    return {};
}

} // namespace

QVector<Post> Scanner::scan(const QString &root, Scanner *owner, bool useCache,
                            QSet<QString> *unknownOut) {
    QVector<Post> posts;
    const QDir rootDir(root);
    if (!rootDir.exists())
        return posts;
    const QString rootAbs = rootDir.absolutePath();

    ScanCache cache;
    if (useCache)
        cache.open(rootAbs, explicitTagsHash()); // failure -> plain scan

    QSet<QString> seen;
    const QStringList &exts = mediaExtensions();

    // Fast counting pass so progress is determinate (no parsing here).
    int total = 0;
    {
        QDirIterator counter(rootAbs, QDir::Files | QDir::NoDotAndDotDot,
                             QDirIterator::Subdirectories);
        while (counter.hasNext()) {
            counter.next();
            if (exts.contains(counter.fileInfo().suffix().toLower()))
                total++;
        }
    }
    if (owner)
        emit owner->progress(0, total);

    QDirIterator it(rootAbs, QDir::Files | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);
    int sinceProgress = 0;
    while (it.hasNext()) {
        it.next();
        const QFileInfo fi = it.fileInfo();
        const QString suffix = fi.suffix().toLower();
        if (!exts.contains(suffix))
            continue;

        const QString relPath = rootDir.relativeFilePath(fi.absoluteFilePath());
        const qint64 mtime = fi.lastModified().toMSecsSinceEpoch();
        const qint64 size = fi.size();

        Post post;
        bool haveMeta = false;
        if (cache.isOpen() && cache.lookup(relPath, mtime, size, post)) {
            haveMeta = post.hasMeta;
        } else {
            // gallery-dl layout first: <mediafile>.json (image.jpg.json)
            const QString flatSidecar = fi.absoluteFilePath() + QStringLiteral(".json");
            // LOSN layout: <stem>.json
            const QString stemSidecar = fi.absolutePath() + u'/' +
                                        fi.completeBaseName() +
                                        QStringLiteral(".json");
            if (QFile::exists(flatSidecar))
                haveMeta = loadSidecar(flatSidecar, post);
            if (!haveMeta && QFile::exists(stemSidecar))
                haveMeta = loadSidecar(stemSidecar, post);
            if (!haveMeta) {
                post.rating = QStringLiteral("general");
                post.hasMeta = false;
            }
            if (cache.isOpen())
                cache.store(relPath, mtime, size, post);
        }

        post.file = relPath;
        post.absFile = fi.absoluteFilePath();
        post.ext = suffix;
        // Metadata wins when it carries a category (old flat format has one),
        // otherwise derive it from the directory layout / filename prefix.
        if (post.category.isEmpty()) {
            QString relDir = rootDir.relativeFilePath(fi.absolutePath());
            QStringList dirParts;
            if (relDir != QStringLiteral("."))
                dirParts = relDir.split(u'/', Qt::SkipEmptyParts);
            post.category = categoryFor(dirParts, fi.fileName());
        }
        posts.append(post);
        seen.insert(relPath);
        if (unknownOut && !post.untyped.isEmpty()) {
            for (const QString &t : post.untyped.split(QChar::Space, Qt::SkipEmptyParts))
                unknownOut->insert(t);
        }

        if (owner && ++sinceProgress >= 200) {
            sinceProgress = 0;
            emit owner->progress(posts.size(), total);
        }
    }
    if (cache.isOpen()) {
        cache.finish(seen);
        if (owner)
            emit owner->cacheStats(cache.hits(), cache.misses());
    }
    if (owner)
        emit owner->progress(posts.size(), total);
    return posts;
}
