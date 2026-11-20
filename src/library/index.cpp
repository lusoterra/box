#include "index.h"
#include "tagtypes.h"

#include <QFileInfo>
#include <QList>
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
    QList<QSet<QString>> incFiletype, excFiletype; // filetype:a,b ext sets
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
        } else if (isFiletypeToken(t)) {
            // Media-kind filter (extension sets, not tags).
            (neg ? excFiletype : incFiletype).append(filetypeExtSet(t));
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
        if (!filetypeExtOk(p.ext, incFiletype, excFiletype))
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
        for (qsizetype i = results.size() - 1; i > 0; --i)
            results.swapItemsAt(
                i, static_cast<qsizetype>(QRandomGenerator::global()->bounded(
                       static_cast<quint32>(i + 1))));
    } else if (sort == QStringLiteral("added")) {
        // File mtime, newest first (download folders: actual arrival order).
        // One stat per post, sorted once.
        QVector<QPair<qint64, qsizetype>> order;
        order.reserve(results.size());
        for (qsizetype i = 0; i < results.size(); ++i)
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
    out.total = static_cast<int>(results.size());
    out.page = qMax(1, page);
    out.perPage = qMax(1, perPage);
    // 64-bit page math: a huge page/perPage pair must land past the end
    // (empty page), never wrap negative and return the wrong slice.
    // perPage stays unclamped: callers pass INT_MAX to mean "all".
    const qint64 start = qint64(out.page - 1) * qint64(out.perPage);
    if (start < results.size())
        out.posts = results.mid(int(start), out.perPage);
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
    // by the user), then prefix matches by use count. Substring matches
    // (what you type is in the middle of the tag) only fill leftover slots
    // AFTER every prefix match: pooling them with prefix hits used to
    // surface random obscure tags for short prefixes.
    struct Hit {
        const SuggestEntry *e;
        int tier; // 0 = exact, 1 = favourite, 2 = prefix
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
    if (hits.size() < limit) {
        // Slow path, bounded: substring hits ranked strictly last, by use
        // count, only to fill the remaining popup slots.
        QVector<const SuggestEntry *> subs;
        for (const SuggestEntry &e : m_suggest) {
            if (e.lower != q && !e.lower.startsWith(q) &&
                e.lower.contains(q))
                subs.append(&e);
        }
        std::stable_sort(subs.begin(), subs.end(),
                         [](const SuggestEntry *a, const SuggestEntry *b) {
                             if (a->count != b->count)
                                 return a->count > b->count;
                             if (a->tag.size() != b->tag.size())
                                 return a->tag.size() > b->tag.size();
                             return a->tag < b->tag;
                         });
        for (const SuggestEntry *e : subs) {
            if (hits.size() >= limit)
                break;
            hits.append({e, 3});
        }
    }
    QVector<Suggestion> out;
    out.reserve(hits.size());
    for (const Hit &h : hits)
        out.append({h.e->tag, h.e->count, h.e->bucket});
    return out;
}

QVector<Index::Suggestion> Index::topTags(int limit) const {
    QVector<Suggestion> out;
    if (limit <= 0)
        return out;
    out.reserve(m_suggest.size());
    for (const SuggestEntry &e : m_suggest) {
        if (e.count < 2 && e.bucket != QStringLiteral("custom"))
            continue; // one-off tags are noise in a shared cache
        out.append({e.tag, e.count, e.bucket});
    }
    std::stable_sort(out.begin(), out.end(),
                     [](const Suggestion &a, const Suggestion &b) {
                         if (a.count != b.count)
                             return a.count > b.count;
                         return a.tag < b.tag;
                     });
    if (out.size() > limit)
        out.resize(limit);
    return out;
}

Stats Index::stats() const {    Stats s;
    s.total = static_cast<int>(m_posts.size());
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
        QStringList removed = m.alsoFrom;
        removed.prepend(m.from);
        for (const QString &from : removed) {
            auto fromIt = m_tagCounts.find(from);
            if (fromIt != m_tagCounts.end()) {
                if (--fromIt.value()[m.tag] <= 0)
                    fromIt.value().remove(m.tag);
            }
        }
        m_tagCounts[m.to][m.tag]++;
    }
    return static_cast<int>(moves.size());
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
        QStringList gen = splitTags(p.tagsGeneral);
        QStringList artist = splitTags(p.tagsArtist);
        QStringList character = splitTags(p.tagsCharacter);
        QStringList copyright = splitTags(p.tagsCopyright);
        QStringList meta = splitTags(p.tagsMeta);
        // Tags to re-bucket: the untyped ones (existing) plus any tag
        // already sitting in a bucket that has a verdict. The latter heals
        // rows where a verdict moved a tag into its bucket but left a stale
        // copy behind (lucky_star in both Copyright and Meta).
        QStringList toProcess = splitTags(p.untyped);
        for (const QString &t : gen + artist + character + copyright + meta) {
            if (tagToBucket.contains(t.toLower()) &&
                !toProcess.contains(t))
                toProcess.append(t);
        }
        if (toProcess.isEmpty() && p.untyped.isEmpty())
            continue;
        bool changed = false;
        for (const QString &t : toProcess) {
            // Lookups are lowercase (fetched verdicts and manual overrides
            // are keyed that way); moves keep the post's original spelling.
            const QString bucket = tagToBucket.value(t.toLower());
            if (bucket.isEmpty() || (!TagTypes::isValidBucket(bucket)))
                continue;
            // A tag must live in exactly one bucket. Imported rows used to
            // carry the same tag in General and untyped, so the old
            // remove-one-bucket logic left a second copy behind (tag shown
            // bold in two sections with split counts).
            const QList<QPair<QString, QStringList *>> all = {
                {QStringLiteral("general"), &gen},
                {QStringLiteral("artist"), &artist},
                {QStringLiteral("character"), &character},
                {QStringLiteral("copyright"), &copyright},
                {QStringLiteral("meta"), &meta},
            };
            QStringList present;
            int occurrences = 0;
            for (const auto &[name, list] : all) {
                const qsizetype n = list->count(t);
                if (n > 0) {
                    occurrences += static_cast<int>(n);
                    present << name;
                }
            }
            if (occurrences == 0)
                continue; // not placed anywhere: never invent tags
            if (occurrences == 1 && present.first() == bucket)
                continue; // already there: rescans are no-ops
            for (const auto &[name, list] : all) {
                Q_UNUSED(name);
                list->removeAll(t);
            }
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
            moves.append({t, present.first(), bucket, present.mid(1)});
        }
        if (!changed)
            continue;
        // Tags that received a verdict leave `untyped` so they are not
        // re-offered to the classifier on the next scan.
        QStringList remainingUntyped;
        for (const QString &t : splitTags(p.untyped))
            if (!tagToBucket.contains(t.toLower()))
                remainingUntyped.append(t);
        p.untyped = remainingUntyped.join(u' ');
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
