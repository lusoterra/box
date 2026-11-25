#include "dedupgroups.h"

#include "phash.h"

#include <QCoreApplication>
#include <QHash>
#include <QMap>
#include <QSet>

#include <algorithm>
#include <functional>

namespace {

struct Bucket {
    QString scope;
    QString kind;
    QVector<DedupFile> rows;
};

qint64 pixelArea(const DedupFile &f) {
    return qint64(f.width) * qint64(f.height);
}

bool betterFirst(const DedupFile &a, const DedupFile &b) {
    const qint64 aa = pixelArea(a);
    const qint64 ab = pixelArea(b);
    if (aa != ab)
        return aa > ab;
    if (a.size != b.size)
        return a.size > b.size;
    if (a.mtime != b.mtime)
        return a.mtime > b.mtime;
    return a.path < b.path;
}

// All 16-bit values at Hamming distance <= radius from `value`.
void forEachWithin16(quint16 value, int radius,
                     const std::function<void(quint16)> &fn) {
    fn(value);
    if (radius <= 0)
        return;
    std::function<void(int, int, quint16)> pick =
        [&](int start, int left, quint16 acc) {
            if (left == 0) {
                fn(acc);
                return;
            }
            for (int bit = start; bit <= 16 - left; ++bit)
                pick(bit + 1, left - 1,
                     static_cast<quint16>(acc ^ (quint16(1) << bit)));
        };
    for (int flips = 1; flips <= radius; ++flips)
        pick(0, flips, value);
}

} // namespace

QString dedupScopeLabel(const QString &scope) {
    if (scope == QStringLiteral("archive"))
        return QCoreApplication::translate("Dedup", "Local archive");
    if (scope == QStringLiteral("download"))
        return QCoreApplication::translate("Dedup", "Downloads / favourites");
    if (scope == QStringLiteral("hydrus"))
        return QCoreApplication::translate("Dedup", "Hydrus");
    return scope;
}

QString dedupKindLabel(const QString &kind) {
    if (kind == QStringLiteral("animated"))
        return QCoreApplication::translate("Dedup", "animated");
    if (kind == QStringLiteral("video"))
        return QCoreApplication::translate("Dedup", "video");
    return QCoreApplication::translate("Dedup", "image");
}

QVector<DedupGroup> buildDedupGroups(const QVector<DedupFile> &rows,
                                     int maxDistance) {
    QVector<DedupGroup> out;
    const int threshold = qBound(0, maxDistance, 16);

    // Partition by scope+kind. Only rows that can actually match are
    // considered: an exact hash or a usable perceptual hash.
    QMap<QString, Bucket> buckets;
    for (const DedupFile &f : rows) {
        if (f.scope.isEmpty() || f.path.isEmpty())
            continue;
        if (f.sha256.isEmpty() && !f.hasPhash)
            continue;
        const QString key = f.scope + u'\n' + f.kind;
        Bucket &b = buckets[key];
        b.scope = f.scope;
        b.kind = f.kind;
        b.rows.append(f);
    }

    for (auto it = buckets.begin(); it != buckets.end(); ++it) {
        Bucket &bucket = it.value();
        std::sort(bucket.rows.begin(), bucket.rows.end(), betterFirst);

        QVector<DedupGroup> groups;
        QHash<QString, int> bySha;
        // Four 16-bit bands: if two hashes are within t bits, at least
        // one band differs by at most floor(t/4) bits (pigeonhole), so a
        // candidate search per band cannot miss a true match.
        QHash<quint16, QVector<int>> bands[4];
        const int bandRadius = threshold / 4;

        auto addToGroup = [&](int gi, const DedupFile &f) {
            groups[gi].files.append(f);
        };

        auto findGroup = [&](const DedupFile &f) -> int {
            if (!f.hasPhash || f.phash == 0)
                return -1;
            QSet<int> candidates;
            for (int b = 0; b < 4; ++b) {
                const quint16 part =
                    static_cast<quint16>((f.phash >> (16 * b)) & 0xffff);
                forEachWithin16(part, bandRadius, [&](quint16 candidate) {
                    const auto hit = bands[b].constFind(candidate);
                    if (hit == bands[b].constEnd())
                        return;
                    for (int gi : hit.value())
                        candidates.insert(gi);
                });
            }
            int best = -1;
            int bestDistance = threshold + 1;
            for (int gi : candidates) {
                const DedupFile &rep = groups[gi].files.first();
                if (!rep.hasPhash)
                    continue;
                const int d = phashDistance(f.phash, rep.phash);
                if (d <= threshold && d < bestDistance) {
                    bestDistance = d;
                    best = gi;
                }
            }
            return best;
        };

        auto indexGroup = [&](int gi) {
            const DedupFile &rep = groups[gi].files.first();
            if (!rep.hasPhash || rep.phash == 0)
                return;
            for (int b = 0; b < 4; ++b) {
                const quint16 part =
                    static_cast<quint16>((rep.phash >> (16 * b)) & 0xffff);
                bands[b][part].append(gi);
            }
        };

        for (const DedupFile &f : bucket.rows) {
            if (!f.sha256.isEmpty()) {
                const auto hit = bySha.constFind(f.sha256);
                if (hit != bySha.constEnd()) {
                    addToGroup(hit.value(), f);
                    continue;
                }
            }
            const int near = findGroup(f);
            if (near >= 0) {
                addToGroup(near, f);
                if (!f.sha256.isEmpty())
                    bySha.insert(f.sha256, near);
                continue;
            }
            DedupGroup g;
            g.scope = bucket.scope;
            g.kind = bucket.kind;
            g.files.append(f);
            groups.append(g);
            const int gi = static_cast<int>(groups.size()) - 1;
            if (!f.sha256.isEmpty())
                bySha.insert(f.sha256, gi);
            indexGroup(gi);
        }

        for (DedupGroup &g : groups) {
            if (g.files.size() < 2)
                continue;
            qint64 total = 0;
            for (const DedupFile &f : g.files)
                total += f.size;
            g.wasted = qMax<qint64>(0, total - g.files.first().size);
            out.append(g);
        }
    }

    std::sort(out.begin(), out.end(), [](const DedupGroup &a,
                                         const DedupGroup &b) {
        if (a.wasted != b.wasted)
            return a.wasted > b.wasted;
        if (a.files.size() != b.files.size())
            return a.files.size() > b.files.size();
        return a.files.first().path < b.files.first().path;
    });
    return out;
}
