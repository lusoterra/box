#include "index.h"

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
        for (const QString &t : splitTags(p.tagsArtist))
            m_tagCounts[QStringLiteral("artist")][t]++;
        for (const QString &t : splitTags(p.tagsCharacter))
            m_tagCounts[QStringLiteral("character")][t]++;
        for (const QString &t : splitTags(p.tagsCopyright))
            m_tagCounts[QStringLiteral("copyright")][t]++;
        for (const QString &t : splitTags(p.tagsGeneral))
            m_tagCounts[QStringLiteral("general")][t]++;
        for (const QString &t : splitTags(p.tagsMeta))
            m_tagCounts[QStringLiteral("meta")][t]++;
        if (!p.category.isEmpty())
            m_categories[p.category]++;
    }
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
    if (q.length() < 2)
        return {};
    QHash<QString, int> combined;
    for (auto bit = m_tagCounts.begin(); bit != m_tagCounts.end(); ++bit) {
        for (auto it = bit.value().begin(); it != bit.value().end(); ++it) {
            if (it.key().toLower().contains(q))
                combined[it.key()] += it.value();
        }
    }
    QVector<Suggestion> out;
    out.reserve(combined.size());
    for (auto it = combined.begin(); it != combined.end(); ++it)
        out.append({it.key(), it.value()});
    std::sort(out.begin(), out.end(),
              [](const Suggestion &a, const Suggestion &b) { return a.count > b.count; });
    if (out.size() > limit)
        out.resize(limit);
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

QSet<QString> Index::collectUntyped() const {
    QSet<QString> out;
    for (const Post &p : m_posts) {
        for (const QString &t : splitTags(p.untyped))
            out.insert(t);
    }
    return out;
}

int Index::applyTagTypes(const QHash<QString, QString> &tagToBucket) {
    if (tagToBucket.isEmpty())
        return 0;
    int moved = 0;
    for (Post &p : m_posts) {
        if (p.untyped.isEmpty())
            continue;
        QStringList gen = splitTags(p.tagsGeneral);
        QStringList artist = splitTags(p.tagsArtist);
        QStringList character = splitTags(p.tagsCharacter);
        QStringList copyright = splitTags(p.tagsCopyright);
        QStringList meta = splitTags(p.tagsMeta);
        bool changed = false;
        for (const QString &t : splitTags(p.untyped)) {
            const QString bucket = tagToBucket.value(t);
            if (bucket.isEmpty() || bucket == QStringLiteral("general"))
                continue;
            if (!gen.removeOne(t))
                continue; // already moved (or not present)
            if (bucket == QStringLiteral("artist"))
                artist << t;
            else if (bucket == QStringLiteral("character"))
                character << t;
            else if (bucket == QStringLiteral("copyright"))
                copyright << t;
            else if (bucket == QStringLiteral("meta"))
                meta << t;
            else
                continue;
            changed = true;
            moved++;
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
    if (moved > 0)
        recount();
    return moved;
}
