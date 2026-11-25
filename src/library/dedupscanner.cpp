#include "dedupscanner.h"

#include "hydrusstore.h"
#include "phash.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSet>

#include <algorithm>

namespace {

bool isUnder(const QString &path, const QString &dir) {
    if (path.isEmpty() || dir.isEmpty())
        return false;
    const QString d = QDir(dir).canonicalPath();
    if (d.isEmpty())
        return false;
    const QString p = QFileInfo(path).canonicalFilePath();
    const QString use = p.isEmpty() ? QFileInfo(path).absoluteFilePath() : p;
    return use == d || use.startsWith(d + u'/');
}

// Both arguments must already be canonical: used once per candidate, so
// it must not hit the filesystem.
bool isUnderCanonical(const QString &path, const QString &dir) {
    return path == dir || path.startsWith(dir + u'/');
}

QString canonicalDir(const QString &path) {
    const QString c = QDir(path).canonicalPath();
    return c.isEmpty() ? QDir(path).absolutePath() : c;
}

QString sha256File(const QString &path, const std::atomic<bool> &cancel,
                   bool *cancelled) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    QCryptographicHash h(QCryptographicHash::Sha256);
    while (!f.atEnd()) {
        if (cancel.load()) {
            if (cancelled)
                *cancelled = true;
            return {};
        }
        const QByteArray chunk = f.read(1 << 20);
        if (chunk.isEmpty())
            break;
        h.addData(chunk);
    }
    if (f.error() != QFileDevice::NoError)
        return {};
    return QString::fromLatin1(h.result().toHex());
}

const QStringList &videoExtensions() {
    static const QStringList exts = {
        QStringLiteral("mp4"), QStringLiteral("webm"),
        QStringLiteral("mkv"), QStringLiteral("avi"),
        QStringLiteral("mov"), QStringLiteral("m4v"),
    };
    return exts;
}

const QStringList &imageExtensions() {
    static const QStringList exts = {
        QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("jpeg"),
        QStringLiteral("gif"), QStringLiteral("webp"), QStringLiteral("avif"),
        QStringLiteral("jxl"), QStringLiteral("bmp"), QStringLiteral("tif"),
        QStringLiteral("tiff"),
    };
    return exts;
}

} // namespace

void DedupScanTask::run(const DedupScanOptions &options) {
    result = Result{};
    DedupDb db;
    if (!db.open(options.dbPath)) {
        result.error = db.lastError().isEmpty()
                           ? QStringLiteral("cannot open the hash database")
                           : db.lastError();
        return;
    }

    const QString downloadDir = canonicalDir(options.downloadDir);
    const QString hydrusDir = canonicalDir(options.hydrusDir);

    // All archive roots, for keeping families apart when directories are
    // nested (e.g. downloads/ living inside an archive root).
    QStringList archiveRoots;
    for (const DedupSource &s : options.sources)
        if (s.scope == QStringLiteral("archive"))
            archiveRoots << canonicalDir(s.root);

    // Hydrus already stores SHA-256 and dimensions: reuse them instead of
    // re-reading every video. Built once for the whole scan.
    QHash<QString, HydrusStore::Record> hydrusByPath;
    if (!options.hydrusDbPath.isEmpty()) {
        HydrusStore::Db hdb;
        if (hdb.open(options.hydrusDbPath)) {
            const QVector<HydrusStore::Record> rows = hdb.list();
            const QString dir = hydrusDir;
            for (const HydrusStore::Record &r : rows) {
                if (r.fileName.isEmpty() || dir.isEmpty())
                    continue;
                hydrusByPath.insert(QDir::cleanPath(dir + u'/' + r.fileName), r);
            }
            hdb.close();
        }
    }

    QHash<QString, QHash<QString, DedupFile>> existing;
    QSet<QString> queuedGlobally;

    const auto mediaExt = [](const QString &ext) {
        return imageExtensions().contains(ext) ||
               videoExtensions().contains(ext);
    };

    for (const DedupSource &source : options.sources) {
        if (cancelRequested.load()) {
            result.cancelled = true;
            return;
        }
        const QString root = canonicalDir(source.root);
        if (root.isEmpty() || !QDir(root).exists()) {
            db.prune(source.scope, root, {});
            continue;
        }

        // A foreign family's folder nested inside this root belongs to
        // its own scope; skip it here. Archive roots nested in another
        // archive are normalised away by the dialog.
        QStringList excludes;
        const auto nestedForeign = [&](const QString &foreign) {
            if (foreign.isEmpty() || foreign == root)
                return;
            if (isUnder(foreign, root))
                excludes << foreign;
        };
        if (source.scope == QStringLiteral("archive")) {
            nestedForeign(downloadDir);
            nestedForeign(hydrusDir);
        } else if (source.scope == QStringLiteral("download")) {
            nestedForeign(hydrusDir);
            for (const QString &a : archiveRoots)
                nestedForeign(a);
        } else if (source.scope == QStringLiteral("hydrus")) {
            nestedForeign(downloadDir);
            for (const QString &a : archiveRoots)
                nestedForeign(a);
        }

        struct Candidate {
            QString path;
            qint64 size = 0;
            qint64 mtime = 0;
            QString shaHint;
            int width = 0;
            int height = 0;
        };
        QVector<Candidate> candidates;
        QSet<QString> seen;
        QDirIterator it(root, QDir::Files | QDir::NoSymLinks | QDir::Readable,
                        QDirIterator::Subdirectories);
        while (it.hasNext()) {
            if (cancelRequested.load()) {
                result.cancelled = true;
                return;
            }
            const QString path = it.next();
            const QFileInfo fi(path);
            if (fi.fileName().startsWith(u'.'))
                continue;
            const QString ext = fi.suffix().toLower();
            if (!mediaExt(ext))
                continue;
            const QString abs =
                fi.canonicalFilePath().isEmpty() ? fi.absoluteFilePath()
                                                 : fi.canonicalFilePath();
            if (abs.isEmpty() || queuedGlobally.contains(abs))
                continue;
            bool excluded = false;
            for (const QString &e : excludes) {
                if (isUnderCanonical(abs, e)) {
                    excluded = true;
                    break;
                }
            }
            if (excluded)
                continue;
            queuedGlobally.insert(abs);
            seen.insert(abs);
            Candidate c;
            c.path = abs;
            c.size = fi.size();
            c.mtime = fi.lastModified().toMSecsSinceEpoch();
            if (source.scope == QStringLiteral("hydrus")) {
                const auto hint = hydrusByPath.constFind(abs);
                if (hint != hydrusByPath.constEnd() &&
                    hint->bytes == fi.size()) {
                    c.shaHint = hint->sha256;
                    c.width = hint->width;
                    c.height = hint->height;
                }
            }
            candidates.append(c);
        }

        if (!existing.contains(source.scope)) {
            QHash<QString, DedupFile> cachedRows;
            for (const DedupFile &f : db.files(source.scope))
                cachedRows.insert(f.path, f);
            existing.insert(source.scope, cachedRows);
        }
        QHash<QString, DedupFile> &known = existing[source.scope];

        result.total += static_cast<int>(candidates.size());
        int done = 0;
        bool inTx = false;
        for (const Candidate &c : candidates) {
            if (cancelRequested.load()) {
                if (inTx)
                    db.rollback();
                result.cancelled = true;
                return;
            }
            const auto old = known.constFind(c.path);
            if (old != known.constEnd() && old->size == c.size &&
                old->mtime == c.mtime && !old->sha256.isEmpty()) {
                ++result.cached;
                ++done;
                if (done % 10 == 0 || done == candidates.size())
                    emit progress(done, candidates.size(), source.scope,
                                  QFileInfo(c.path).fileName());
                continue;
            }

            DedupFile f;
            bool cancelled = false;
            if (!hashFile(c.path, source.scope, root, c.shaHint, c.width,
                          c.height, &f, &cancelled)) {
                if (inTx)
                    db.rollback();
                if (cancelled) {
                    result.cancelled = true;
                    return;
                }
                // A changed file that cannot be read must not keep its
                // previous hash: that stale value could join a wrong
                // group. The next readable scan re-adds it.
                if (known.contains(c.path)) {
                    db.removePath(source.scope, c.path);
                    known.remove(c.path);
                }
                ++result.failed;
                ++done;
                continue;
            }
            f.id = old != known.constEnd() ? old->id : 0;
            if (!inTx) {
                inTx = db.begin();
            }
            if (!db.upsert(f)) {
                if (inTx)
                    db.rollback();
                result.error = db.lastError();
                return;
            }
            known.insert(f.path, f);
            ++result.hashed;
            ++done;
            if (inTx && result.hashed % 100 == 0) {
                if (!db.commit()) {
                    result.error = db.lastError();
                    return;
                }
                inTx = false;
            }
            if (done % 10 == 0 || done == candidates.size())
                emit progress(done, candidates.size(), source.scope,
                              QFileInfo(c.path).fileName());
        }
        if (inTx) {
            if (!db.commit()) {
                result.error = db.lastError();
                return;
            }
            inTx = false;
        }
        db.prune(source.scope, root, seen);
    }

    db.close();
}

bool DedupScanTask::hashFile(const QString &path, const QString &scope,
                             const QString &root, const QString &shaHint,
                             int widthHint, int heightHint, DedupFile *out,
                             bool *cancelled) {
    if (cancelled)
        *cancelled = false;
    const QFileInfo fi(path);
    const QString ext = fi.suffix().toLower();
    const bool video = videoExtensions().contains(ext);

    DedupFile f;
    f.scope = scope;
    f.root = root;
    f.path = path;
    f.size = fi.size();
    f.mtime = fi.lastModified().toMSecsSinceEpoch();
    f.hashedMs = QDateTime::currentMSecsSinceEpoch();

    if (video) {
        f.kind = QStringLiteral("video");
        f.width = qMax(0, widthHint);
        f.height = qMax(0, heightHint);
        f.hasPhash = false;
        f.sha256 = shaHint;
        if (f.sha256.isEmpty())
            f.sha256 = sha256File(path, cancelRequested, cancelled);
    } else {
        const PixelHash ph = perceptualHashFile(path);
        f.width = ph.width > 0 ? ph.width : qMax(0, widthHint);
        f.height = ph.height > 0 ? ph.height : qMax(0, heightHint);
        f.phash = ph.hash;
        // A zero hash means a flat frame (no usable perceptual signal) or
        // a decode failure: keep it out of near-duplicate matching.
        f.hasPhash = ph.ok && ph.hash != 0;
        f.kind = ph.animated ? QStringLiteral("animated")
                             : QStringLiteral("image");
        if (!ph.ok && ext == QStringLiteral("gif"))
            f.kind = QStringLiteral("animated");
        f.sha256 = shaHint;
        if (f.sha256.isEmpty())
            f.sha256 = sha256File(path, cancelRequested, cancelled);
    }
    if (shaHint.isEmpty() && f.sha256.isEmpty())
        return false;
    *out = f;
    return true;
}
