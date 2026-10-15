#include "index.h"
#include "tagtypes.h"

#include <QFileInfo>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSet>
#include <algorithm>

namespace {
const QRegularExpression kWs(QStringLiteral("\\s+"));

QStringList splitTags(const QString &s) {
    return s.split(kWs, Qt::SkipEmptyParts);
}

QVector<QPair<QString, int>> topN(const QHash<QString, int> &m, int n) {
    QVector<QPair<QString, int>> v;
    v.reserve(m.size());
    for (auto it = m.begin(); it != m.end(); ++it)
        v.append({it.key(), it.value()});
    std::sort(v.begin(), v.end(),
              [](const auto &a, const auto &b) { return a.second > b.second; });
    if (v.size() > n)
        v.resize(n);
    return v;
}
} // namespace

void Index::setPosts(QVector<Post> posts) {
    m_posts = std::move(posts);
    recount();
}

void Index::recount() {
    m_tagCounts = {
        {QStringLiteral("artist"), {}},   {QStringLiteral("character"), {}},
        {QStringLiteral("copyright"), {}}, {QStringLiteral("general"), {}},
        {QStringLiteral("meta"), {}},
    };
    m_categories.clear();
    for (const Post &p : m_posts) {
        for (const QString &t : cleanTagTokens(splitTags(p.tagsArtist)))
            m_tagCounts[QStringLiteral("artist")][t]++;
        for (const QString &t : cleanTagTokens(splitTags(p.tagsCharacter)))
            m_tagCounts[QStringLiteral("character")][t]++;
        for (const QString &t : cleanTagTokens(splitTags(p.tagsCopyright)))
            m_tagCounts[QStringLiteral("copyright")][t]++;
        for (const QString &t : cleanTagTokens(splitTags(p.tagsGeneral)))
            m_tagCounts[QStringLiteral("general")][t]++;
        for (const QString &t : cleanTagTokens(splitTags(p.tagsMeta)))
            m_tagCounts[QStringLiteral("meta")][t]++;
        if (!p.category.isEmpty())
            m_categories[p.category]++;
    }
    // Merge buckets once: dominant bucket wins for colors, counts add up.
    struct Agg {
        QString tag;
        int count = 0;
        QString bucket;
        int bucketCount = 0;
    };
    QHash<QString, Agg> agg;
    for (auto bit = m_tagCounts.begin(); bit != m_tagCounts.end(); ++bit) {
        for (auto it = bit.value().begin(); it != bit.value().end(); ++it) {
            Agg &a = agg[it.key().toLower()];
            a.tag = it.key();
            a.count += it.value();
            if (it.value() > a.bucketCount) {
                a.bucketCount = it.value();
                a.bucket = bit.key();
            }
        }
    }
    m_suggest.clear();
    m_suggest.reserve(agg.size());
    for (auto it = agg.begin(); it != agg.end(); ++it)
        m_suggest.append({it.key(), it.value().tag, it.value().count,
                          it.value().bucket});
}

QueryResult Index::query(const QStringList &tags, const QString &rating,
                         const QString &category, const QString &sort,
                         int page, int perPage) const {
    QStringList inc, exc;
    for (const QString &t : tags) {
        if (t.startsWith(u'-') && t.size() > 1)
            exc << t.mid(1);
        else if (!t.isEmpty())
            inc << t;
    }

    QVector<Post> results;
    results.reserve(m_posts.size());
    for (const Post &p : m_posts) {
        if (rating != QStringLiteral("all") && p.rating != rating)
            continue;
        if (!category.isEmpty() && p.category != category)
            continue;
        if (!inc.isEmpty() || !exc.isEmpty()) {
            const QStringList pt = splitTags(p.tags);
            bool ok = true;
            for (const QString &t : inc) {
                if (!pt.contains(t)) { ok = false; break; }
            }
            if (ok) {
                for (const QString &t : exc) {
                    if (pt.contains(t)) { ok = false; break; }
                }
            }
            if (!ok)
                continue;
        }
        results.append(p);
    }

    if (sort == QStringLiteral("id-asc"))
        std::sort(results.begin(), results.end(),
                  [](const Post &a, const Post &b) { return a.id < b.id; });
    else if (sort == QStringLiteral("score-desc"))
        std::sort(results.begin(), results.end(),
                  [](const Post &a, const Post &b) { return a.score > b.score; });
    else if (sort == QStringLiteral("score-asc"))
        std::sort(results.begin(), results.end(),
                  [](const Post &a, const Post &b) { return a.score < b.score; });
    else if (sort == QStringLiteral("random")) {
        for (int i = results.size() - 1; i > 0; --i)
            results.swapItemsAt(i, static_cast<int>(
                                        QRandomGenerator::global()->bounded(i + 1)));
    } else if (sort == QStringLiteral("added")) {
        // File mtime, newest first (download folders: actual arrival order).
        // One stat per post, sorted once.
        QVector<QPair<qint64, int>> order;
        order.reserve(results.size());
        for (int i = 0; i < results.size(); ++i)
            order.append({QFileInfo(results.at(i).absFile).lastModified().toMSecsSinceEpoch(), i});
        std::sort(order.begin(), order.end(),
                  [](const auto &a, const auto &b) { return a.first > b.first; });
        QVector<Post> sorted;
        sorted.reserve(results.size());
        for (const auto &e : order)
            sorted.append(results.at(e.second));
        results.swap(sorted);
    } else { // id-desc default
        std::sort(results.begin(), results.end(),
                  [](const Post &a, const Post &b) { return a.id > b.id; });
    }

    QueryResult out;
    out.total = results.size();
    out.page = qMax(1, page);
    out.perPage = qMax(1, perPage);
    const int start = (out.page - 1) * out.perPage;
    if (start < results.size())
        out.posts = results.mid(start, out.perPage);
    return out;
}

QVector<Index::Suggestion> Index::suggest(const QString &prefix, int limit) const {
    const QString q = prefix.toLower();
    if (q.length() < 2 || limit <= 0)
        return {};
    // Prefix matches first (binary-searchable order would need sorting;
    // a single linear scan over precomputed entries is fast enough and
    // keeps substring behavior for non-prefix hits).
    QVector<const SuggestEntry *> hits;
    hits.reserve(qMax(0, limit * 2));
    for (const SuggestEntry &e : m_suggest) {
        if (e.lower.startsWith(q))
            hits.append(&e);
    }
    if (hits.size() < limit) {
        for (const SuggestEntry &e : m_suggest) {
            if (!e.lower.startsWith(q) && e.lower.contains(q)) {
                hits.append(&e);
                if (hits.size() >= limit * 4)
                    break; // bound the slow path; top-N trims below
            }
        }
    }
    if (hits.isEmpty())
        return {};
    std::nth_element(hits.begin(), hits.begin() + qMin(limit, hits.size()) - 1,
                     hits.end(), [](const SuggestEntry *a, const SuggestEntry *b) {
                         return a->count > b->count;
                     });
    if (hits.size() > limit)
        hits.resize(limit);
    std::sort(hits.begin(), hits.end(), [](const SuggestEntry *a,
                                           const SuggestEntry *b) {
        return a->count > b->count;
    });
    QVector<Suggestion> out;
    out.reserve(hits.size());
    for (const SuggestEntry *e : hits)
        out.append({e->tag, e->count, e->bucket});
    return out;
}

Stats Index::stats() const {    Stats s;
    s.total = m_posts.size();
    for (const Post &p : m_posts)
        s.ratings[p.rating]++;
    s.categories = m_categories;
    s.topArtists = topN(m_tagCounts.value(QStringLiteral("artist")), 30);
    s.topCharacters = topN(m_tagCounts.value(QStringLiteral("character")), 30);
    s.topCopyrights = topN(m_tagCounts.value(QStringLiteral("copyright")), 30);
    s.topGeneral = topN(m_tagCounts.value(QStringLiteral("general")), 50);
    s.topMeta = topN(m_tagCounts.value(QStringLiteral("meta")), 30);
    return s;
}

QString Index::bucketOf(const QString &tag) const {
    for (const char *bucket :
         {"artist", "character", "copyright", "meta", "general"}) {
        const QString b = QString::fromLatin1(bucket);
        auto it = m_tagCounts.find(b);
        if (it != m_tagCounts.end() && it.value().contains(tag))
            return b;
    }
    return QStringLiteral("general");
}

QSet<QString> Index::collectUntyped() const {
    QSet<QString> out;
    for (const Post &p : m_posts) {
        for (const QString &t : splitTags(p.untyped))
            out.insert(t.toLower()); // caches/results are lowercase: match them
    }
    return out;
}

int Index::applyTagTypes(const QHash<QString, QString> &tagToBucket) {
    if (tagToBucket.isEmpty())
        return 0;
    const QVector<TagMove> moves = applyTagBuckets(m_posts, tagToBucket);
    // Incremental tally: O(moved), not O(archive).
    for (const TagMove &m : moves) {
        auto fromIt = m_tagCounts.find(m.from);
        if (fromIt != m_tagCounts.end()) {
            if (--fromIt.value()[m.tag] <= 0)
                fromIt.value().remove(m.tag);
        }
        m_tagCounts[m.to][m.tag]++;
    }
    return moves.size();
}

// Shared core with Index::applyTagTypes, for post vectors that live outside
// an Index (browse results). Same contract: case-insensitive lookups,
// override-wins relocation, idempotent, combined set unchanged.
QVector<TagMove> applyTagBuckets(QVector<Post> &posts,
                                 const QHash<QString, QString> &tagToBucket) {
    QVector<TagMove> moves;
    if (tagToBucket.isEmpty())
        return moves;
    for (Post &p : posts) {
        if (p.untyped.isEmpty())
            continue;
        QStringList gen = splitTags(p.tagsGeneral);
        QStringList artist = splitTags(p.tagsArtist);
        QStringList character = splitTags(p.tagsCharacter);
        QStringList copyright = splitTags(p.tagsCopyright);
        QStringList meta = splitTags(p.tagsMeta);
        bool changed = false;
        for (const QString &t : splitTags(p.untyped)) {
            // Lookups are lowercase (fetched verdicts and manual overrides
            // are keyed that way); moves keep the post's original spelling.
            const QString bucket = tagToBucket.value(t.toLower());
            if (bucket.isEmpty() || (!TagTypes::isValidBucket(bucket)))
                continue;
            QStringList *placed = nullptr;
            QString placedName;
            if (gen.contains(t)) {
                placed = &gen;
                placedName = QStringLiteral("general");
            } else if (artist.contains(t)) {
                placed = &artist;
                placedName = QStringLiteral("artist");
            } else if (character.contains(t)) {
                placed = &character;
                placedName = QStringLiteral("character");
            } else if (copyright.contains(t)) {
                placed = &copyright;
                placedName = QStringLiteral("copyright");
            } else if (meta.contains(t)) {
                placed = &meta;
                placedName = QStringLiteral("meta");
            } else {
                continue; // not placed anywhere: never invent tags
            }
            if (placedName == bucket)
                continue; // already there: rescans are no-ops
            placed->removeOne(t);
            if (bucket == QStringLiteral("general"))
                gen << t;
            else if (bucket == QStringLiteral("artist"))
                artist << t;
            else if (bucket == QStringLiteral("character"))
                character << t;
            else if (bucket == QStringLiteral("copyright"))
                copyright << t;
            else if (bucket == QStringLiteral("meta"))
                meta << t;
            changed = true;
            moves.append({t, placedName, bucket});
        }
        if (!changed)
            continue;
        p.tagsGeneral = gen.join(u' ');
        p.tagsArtist = artist.join(u' ');
        p.tagsCharacter = character.join(u' ');
        p.tagsCopyright = copyright.join(u' ');
        p.tagsMeta = meta.join(u' ');
        p.tags = (artist + character + copyright + gen + meta).join(u' ');
        // NOTE: combined tag set is identical, so rating inference is stable.
    }
    return moves;
}
