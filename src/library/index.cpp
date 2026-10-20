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
    // Native custom categories suggest too (bucket "custom" colors them);
    // counts stay 1-ish so real buckets outrank them.
    for (const Post &p : m_posts) {
        for (auto it = p.customTags.begin(); it != p.customTags.end();
             ++it) {
            for (const QString &t : it.value()) {
                Agg &a = agg[t.toLower()];
                if (a.tag.isEmpty()) {
                    a.tag = t;
                    a.bucket = QStringLiteral("custom");
                }
                a.count += 1;
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
    QStringList incRating, excRating; // rating:g/s/q/e (and safety:) filter
    auto ratingValue = [](const QString &tok) { // the post's rating field
        const QString v = tok.mid(tok.indexOf(u':') + 1).toLower();
        if (v == QStringLiteral("general") || v == QStringLiteral("g") ||
            v == QStringLiteral("safe"))
            return QStringLiteral("general"); // danbooru conventions
        if (v == QStringLiteral("sensitive") || v == QStringLiteral("s"))
            return QStringLiteral("sensitive");
        if (v == QStringLiteral("questionable") || v == QStringLiteral("q"))
            return QStringLiteral("questionable");
        if (v == QStringLiteral("explicit") || v == QStringLiteral("e"))
            return QStringLiteral("explicit");
        return QString(); // unrecognized: keep literal handling below
    };
    for (const QString &t : tags) {
        const bool neg = t.startsWith(u'-') && t.size() > 1;
        const QString tok = neg ? t.mid(1) : t;
        const QString low = tok.toLower();
        const bool isRate = low.startsWith(QStringLiteral("rating:")) ||
                            low.startsWith(QStringLiteral("safety:"));
        const QString rv = isRate ? ratingValue(tok) : QString();
        if (isRate && !rv.isEmpty()) {
            (neg ? excRating : incRating) << rv;
        } else if (neg) {
            exc << t.mid(1);
        } else if (!t.isEmpty()) {
            inc << t;
        }
    }

    QVector<Post> results;
    results.reserve(m_posts.size());
    for (const Post &p : m_posts) {
        if (rating != QStringLiteral("all") && p.rating != rating)
            continue;
        if (!category.isEmpty() && p.category != category)
            continue;
        bool rateOk = true;
        for (const QString &r : incRating) {
            if (p.rating != r) {
                rateOk = false;
                break;
            }
        }
        if (rateOk) {
            for (const QString &r : excRating) {
                if (p.rating == r) {
                    rateOk = false;
                    break;
                }
            }
        }
        if (!rateOk)
            continue;
        if (!inc.isEmpty() || !exc.isEmpty()) {
            // Set lookup, split once: the old per-token contains() on a
            // fresh split was the bulk of filter time on big archives.
            const QStringList toks = splitTags(p.tags);
            const QSet<QString> pt(toks.begin(), toks.end());
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

QVector<Index::Suggestion> Index::suggest(const QString &prefix, int limit,
                                             const QSet<QString> &favTags) const {
    const QString q = prefix.toLower();
    // Defense in depth: limit arithmetic below multiplies, so clamp the
    // top (callers pass 24: a wider pool for ranking, trimmed to the
    // popup cap downstream). Zero/negative still means empty.
    if (q.length() < 2 || limit <= 0)
        return {};
    limit = qMin(limit, 1000);
    // Relevance tiers: the exact tag first, then favourite tags (starred
    // by the user), then prefix matches by use count, then substring
    // matches (longer, more specific tags beat short noise at equal
    // counts). A linear scan over precomputed entries is fast enough; the
    // omnibar completer prefix-filters the model, so substring hits rank
    // strictly last.
    struct Hit {
        const SuggestEntry *e;
        int tier; // 0 = exact, 1 = favourite, 2 = prefix, 3 = substring
    };
    QVector<Hit> hits;
    hits.reserve(qMax(0, limit * 2));
    for (const SuggestEntry &e : m_suggest) {
        int tier = -1;
        if (e.lower == q)
            tier = 0;
        else if (favTags.contains(e.lower))
            tier = e.lower.startsWith(q) ? 1 : -1; // favs must still match
        else if (e.lower.startsWith(q))
            tier = 2;
        if (tier >= 0)
            hits.append({&e, tier});
    }
    if (hits.size() < limit) {
        for (const SuggestEntry &e : m_suggest) {
            if (e.lower != q && !e.lower.startsWith(q) &&
                e.lower.contains(q)) {
                hits.append({&e, 2});
                if (hits.size() >= limit * 4)
                    break; // bound the slow path; ranking trims below
            }
        }
    }
    if (hits.isEmpty())
        return {};
    std::stable_sort(hits.begin(), hits.end(), [](const Hit &a, const Hit &b) {
        if (a.tier != b.tier)
            return a.tier < b.tier;
        if (a.e->count != b.e->count)
            return a.e->count > b.e->count;
        if (a.e->tag.size() != b.e->tag.size())
            return a.e->tag.size() > b.e->tag.size();
        return a.e->tag < b.e->tag;
    });
    if (hits.size() > limit)
        hits.resize(limit);
    QVector<Suggestion> out;
    out.reserve(hits.size());
    for (const Hit &h : hits)
        out.append({h.e->tag, h.e->count, h.e->bucket});
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
