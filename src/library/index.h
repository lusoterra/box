#pragma once

#include "post.h"

#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

struct QueryResult {
    int total = 0;
    int page = 1;
    int perPage = 30;
    QVector<Post> posts; // pointers into Index storage would dangle on rescan,
                         // so filtered copies are returned (tags already interned)
};

struct Stats {
    int total = 0;
    QHash<QString, int> ratings;
    QHash<QString, int> categories;
    QVector<QPair<QString, int>> topArtists;
    QVector<QPair<QString, int>> topCharacters;
    QVector<QPair<QString, int>> topCopyrights;
    QVector<QPair<QString, int>> topGeneral;
    QVector<QPair<QString, int>> topMeta;
};

// In-memory index mirroring the Python prototype's Index class.
class Index {
public:
    void setPosts(QVector<Post> posts);

    const QVector<Post> &posts() const { return m_posts; }
    const QHash<QString, QHash<QString, int>> &tagCounts() const { return m_tagCounts; }
    const QHash<QString, int> &categories() const { return m_categories; }

    QueryResult query(const QStringList &tags, const QString &rating,
                      const QString &category, const QString &sort,
                      int page, int perPage) const;

    struct Suggestion {
        QString tag;
        int count = 0;
        QString bucket = QStringLiteral("general"); // for suggestion colors
    };
    QVector<Suggestion> suggest(const QString &prefix, int limit = 15,
                               const QSet<QString> &favTags = {}) const;

    Stats stats() const;

    // All tags that arrived without a category (Post.untyped), for the
    // optional online classifier.
    QSet<QString> collectUntyped() const;
    // Bucket a tag already lives in ("general" when unknown).
    QString bucketOf(const QString &tag) const;

    // Move untyped tags into their resolved buckets (tag -> bucket name as in
    // TagTypes::bucketForType, or manual overrides). Lookups are
    // case-insensitive; a tag already in its resolved bucket (or pinned to
    // General) is left alone, so rescans are no-ops. Later verdicts win:
    // a tag moved before is relocated when the new bucket differs.
    // The combined tag set is unchanged, so ratings are untouched.
    // Returns the number of moved tags.
    int applyTagTypes(const QHash<QString, QString> &tagToBucket);
private:
    void recount();
    QVector<Post> m_posts;
    // Pre-merged lowercase suggest entries (built by recount so keystrokes
    // scan without hashing, lowercasing, or full sorts).
    struct SuggestEntry {
        QString lower;
        QString tag;
        int count = 0;
        QString bucket;
    };
    QVector<SuggestEntry> m_suggest;
    // bucket -> tag -> count ("artist","character","copyright","general","meta")
    QHash<QString, QHash<QString, int>> m_tagCounts;
    QHash<QString, int> m_categories;
};

// One relocated tag (original spelling + buckets, for count updates).
struct TagMove {
    QString tag;
    QString from;
    QString to;
};
// Free-function core of Index::applyTagTypes for post vectors living outside
// an Index (browse results): same contract, no recount. Returns every move
// so Index can update its tallies incrementally instead of re-scanning.
QVector<TagMove> applyTagBuckets(QVector<Post> &posts,
                                 const QHash<QString, QString> &tagToBucket);
