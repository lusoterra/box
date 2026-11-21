#pragma once

// Persistent tag vocabulary with post counts (SQLite, data dir).
//
// Every tag-API answer the omnibar receives is stored here - spelling,
// category bucket and post count. The suggestion path reads it back
// instantly, ranked by popularity, so a remote tab shows popular matches
// while the network request is still in flight (and keeps working when
// the host is slow, walled or offline).
//
// `count` is the booru's post count and never shrinks: a later answer
// with a smaller count (or none) keeps the higher value, so a tag that is
// genuinely popular stays at the top. Rows keyed by the lowercase
// spelling and trimmed back when the table grows past its cap. Zero-post
// rows are pruned on open (aliases/empties reported with count 0 never
// complete a search; anything real is re-reported with its count).
#include <QSet>
#include <QString>
#include <QVector>

struct CachedTag {
    QString tag;    // canonical spelling as the API returned it
    QString bucket; // suggestion colour bucket
    int count = 0;  // posts using this tag
};

class TagCountCache {
public:
    TagCountCache() = default;
    ~TagCountCache() { close(); }
    TagCountCache(const TagCountCache &) = delete;
    TagCountCache &operator=(const TagCountCache &) = delete;

    // Opens (creating or migrating) the DB. False on error: callers then
    // simply work without the cache.
    bool open(const QString &dbPath);
    bool isOpen() const { return m_open; }
    void close();

    // Upsert answer rows. Counts keep their maximum, the newest bucket and
    // spelling win (case/category fixes reach old rows).
    void upsert(const QVector<CachedTag> &rows);
    // Prefix query: favourite tags first, then post count (desc), then the
    // shorter spelling, then alphabetical. Prefix-only (substring noise
    // never belongs in a completion list).
    QVector<CachedTag> suggest(const QString &prefix, int limit,
                               const QSet<QString> &favTags = {}) const;
    int size() const; // stored rows (tests / status)

    static QString dbPath(const QString &dataDir);

    // Table cap: the least popular rows are dropped beyond this.
    static constexpr int kMaxRows = 200000;

private:
    bool m_open = false;
    QString m_conn;
};
