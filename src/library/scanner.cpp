#include "scanner.h"
#include "cache.h"
#include "explicittags.h"
#include "filehash.h"
#include "metaparser.h"

#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
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
    // Bounded: sidecars are KBs; a multi-GB hostile .json next to media
    // must not OOM the scan.
    static constexpr qint64 kMaxSidecar = 8LL * 1024 * 1024;
    if (f.size() > kMaxSidecar)
        return false;
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

// One-time repair for sidecar-backed files whose names carry no content
// hash (Site_losn123 from imports, Site_456 from id fallback): reuse the
// sidecar's md5, or hash the bytes when it has none, then rename to
// <Site>_<md5>.<ext> and patch the sidecar. Returns the current absolute
// media path (renamed or not). Sidecar-less files are left alone: with no
// provenance the site prefix would be a guess.
QString fixupMd5Name(const QString &absPath, const QString &siteLabel,
                     Scanner *owner) {
    const QFileInfo fi(absPath);
    const QString stem = fi.completeBaseName();
    if (isMd5Stem(stem))
        return absPath;
    const QString dir = fi.absolutePath();
    const QString flatSidecar = absPath + QStringLiteral(".json");
    const QString stemSidecar = dir + u'/' + stem + QStringLiteral(".json");
    const bool hasFlat = QFile::exists(flatSidecar);
    const bool hasStem = QFile::exists(stemSidecar);
    if (!hasFlat && !hasStem)
        return absPath;
    // The sidecar we pair with (flat wins, like the scan below).
    const QString srcSidecar = hasFlat ? flatSidecar : stemSidecar;
    QJsonObject sobj;
    {
        QFile sf(srcSidecar);
        if (sf.open(QIODevice::ReadOnly)) {
            QJsonParseError err;
            const QJsonDocument doc =
                QJsonDocument::fromJson(sf.readAll(), &err);
            if (err.error == QJsonParseError::NoError && doc.isObject())
                sobj = doc.object();
        }
    }
    QString md5 = sobj.value(QStringLiteral("md5")).toString();
    if (md5.isEmpty())
        md5 = sobj.value(QStringLiteral("checksumMD5")).toString();
    bool patched = false;
    // Hex only: a hostile sidecar md5 ("../../x") must never become a
    // filename — treat it as missing and hash the bytes instead.
    bool md5ok = md5.size() == 32;
    for (const QChar &c : md5) {
        const ushort u = c.unicode();
        if (!((u >= u'0' && u <= u'9') || (u >= u'a' && u <= u'f') ||
              (u >= u'A' && u <= u'F')))
            md5ok = false;
    }
    if (!md5ok)
        md5.clear();
    if (md5.isEmpty()) {
        if (owner && owner->cancelRequested.load())
            return absPath;
        md5 = fileContentMd5(absPath);
        if (md5.isEmpty())
            return absPath;
        sobj[QStringLiteral("md5")] = md5;
        patched = true;
    }
    QString safe = siteLabel;
    safe.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9]+")), QString());
    if (safe.isEmpty())
        safe = QStringLiteral("Booru");
    const QString newStem = safe + u'_' + md5;
    auto writePatched = [&](const QString &path) {
        QFile nf(path);
        if (nf.open(QIODevice::WriteOnly | QIODevice::Truncate))
            nf.write(QJsonDocument(sobj).toJson(QJsonDocument::Indented));
    };
    if (newStem == stem) {
        if (patched)
            writePatched(srcSidecar);
        return absPath;
    }
    // Preserve the original extension's case.
    const QString fileName = fi.fileName();
    const QString newAbs =
        dir + u'/' + newStem + fileName.mid(fileName.lastIndexOf(u'.'));
    if (QFile::exists(newAbs) || !QFile::rename(absPath, newAbs)) {
        // Collision or cross-device failure: keep the md5 in the old
        // sidecar at least, so the file stays searchable in-app.
        if (patched)
            writePatched(srcSidecar);
        return absPath;
    }
    const QString newFlat = newAbs + QStringLiteral(".json");
    const QString newStemSide = dir + u'/' + newStem + QStringLiteral(".json");
    auto moveSidecar = [&](const QString &oldP, const QString &newP) {
        if (!QFile::exists(oldP) || oldP == newP)
            return;
        if (patched && oldP == srcSidecar) {
            writePatched(newP);
            QFile::remove(oldP);
        } else {
            QFile::rename(oldP, newP);
        }
    };
    if (hasFlat)
        moveSidecar(flatSidecar, newFlat);
    if (hasStem)
        moveSidecar(stemSidecar, newStemSide);
    return newAbs;
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

    // Snapshot pass: collect every media path BEFORE parsing. The md5
    // repair below renames files on disk, and renaming mid-iteration makes
    // the iterator re-yield the new name (grid showed duplicate cells
    // pointing at the same file after a _losn import). Iterating a frozen
    // list keeps each file to exactly one post; the seen-set stays as a
    // backstop. This also replaces the old counting walk: one traversal.
    QStringList mediaPaths;
    {
        QDirIterator counter(rootAbs, QDir::Files | QDir::NoDotAndDotDot,
                             QDirIterator::Subdirectories);
        while (counter.hasNext()) {
            counter.next();
            if (exts.contains(counter.fileInfo().suffix().toLower()))
                mediaPaths.append(counter.filePath());
        }
    }
    const int total = mediaPaths.size();
    if (owner)
        emit owner->progress(0, total);

    int sinceProgress = 0;
    for (const QString &absIn : mediaPaths) {
        // The file may have been renamed by an earlier repair only in
        // pathological collision cases; never emit a path twice.
        if (!QFile::exists(absIn))
            continue;
        QFileInfo fi(absIn);
        const QString suffix = fi.suffix().toLower();
        if (!exts.contains(suffix))
            continue;

        // One-time md5 repair (may rename the file + sidecar on disk).
        {
            const QString relDir0 =
                rootDir.relativeFilePath(fi.absolutePath());
            const QString siteLabel =
                relDir0 != QStringLiteral(".")
                    ? relDir0.split(u'/', Qt::SkipEmptyParts).value(0)
                    : fi.completeBaseName().section(u'_', 0, 0);
            const QString fixed = fixupMd5Name(fi.absoluteFilePath(),
                                               siteLabel, owner);
            if (fixed != fi.absoluteFilePath())
                fi = QFileInfo(fixed);
        }

        const QString relPath = rootDir.relativeFilePath(fi.absoluteFilePath());
        if (seen.contains(relPath))
            continue; // rename re-yield backstop (see snapshot pass above)
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
            // Cooperative cancel (quit / rescan): return what we have.
            if (owner->cancelRequested.load())
                return posts;
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
