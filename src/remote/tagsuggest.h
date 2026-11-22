#pragma once

// Tag autocomplete for browse tabs: per-engine tag APIs, debounced by the
// caller, one in-flight request at a time, results cached per site+prefix.
// Danbooru categories and Gelbooru/Moebooru types share danbooru numbering
// (0=general 1=artist 3=copyright 4=character 5=meta); Szurubooru categories
// map like post tags. Shimmie exposes no tag API: always empty.
#include <QObject>
#include <QHash>
#include <QSet>
#include <QString>
#include <QVector>

#include "remotesource.h" // BooruEngine/BooruSite/SiteCreds

struct TagSuggestion {
    QString tag;
    QString bucket = QStringLiteral("general"); // suggestion colors
    int count = 0;
};

class Index;
class TagCountCache;

class TagSuggester : public QObject {
    Q_OBJECT
public:
    explicit TagSuggester(QObject *parent = nullptr);

    void suggest(const BooruSite &site, const SiteCreds &creds,
                 const QString &prefix);
    void cancel();
    // Local archive index: its matches emit instantly on every keystroke
    // (offline completion from your own vocabulary); the remote fetch
    // MERGES into them when it lands (never replaces). Not owned.
    void setLocalIndex(const Index *index);
    // Persistent vocabulary filled from tag-API answers: answers keystrokes
    // instantly (ranked by post count) while the network request is out.
    // Not owned.
    void setTagCache(TagCountCache *cache) { m_tagCache = cache; }
    // Starred tags (lowercased by the caller): they outrank plain prefix
    // matches in both the instant local answers and the merged remote ones.
    void setFavTags(const QSet<QString> &fav) { m_favTags = fav; }
    // Recent search tokens (most-recent-first, caller-capped): appended
    // after index rows so something familiar always shows, even offline
    // or where the engine has no suggest API.
    void setHistoryTags(const QStringList &hist) { m_historyTags = hist; }
    // History matches for one prefix (case-insensitive start match),
    // skipping already-listed tags; buckets resolved via index when
    // available, else general. Pure-ish (index read-only).
    static void appendHistory(QVector<TagSuggestion> &out,
                              const QString &bare,
                              const QStringList &history,
                              const Index *index, int cap);

    static QVector<TagSuggestion> parse(BooruEngine engine, const QByteArray &body);
    // Compact popularity label for the suggestion popup: 999, 1.2k, 7k,
    // 1.5M, 20M. 0 = unknown, rendered as an empty string.
    static QString formatCount(qint64 count);
    // Cached popularity rows older than this are not used as instant
    // answers (the host re-confirms them on the next keystroke).
    static constexpr qint64 kFreshSeconds = 7 * 24 * 60 * 60;
    // Merge policy for one prefix: union, case-insensitive dedup (the
    // higher post count and the non-general bucket survive a dup), then
    // ONE global relevance pass, truncated to cap. A rich
    // local vocabulary must never starve server hits (the old
    // locals-first fill dropped ranked remote answers entirely).
    static QVector<TagSuggestion> merge(const QString &prefix,
                                        QVector<TagSuggestion> local,
                                        QVector<TagSuggestion> remote,
                                        const QSet<QString> &favTags = {},
                                        int cap = 20);
    // Local meta-tag completions (no network): when the bare token is a
    // metatag head (`sort:`/`rating:`/`order:`/`filetype:`), complete its
    // value from the engine's verified spellings (Nuuru's native sort:
    // set, rating: shorthands, Danbooru order:/filetype: mirrors of what
    // sortSearchTag() would send). Sorted; empty when the engine strips
    // that head server-side anyway. Pure (unit-tested).
    static QVector<TagSuggestion> metaFor(const QString &bare,
                                          BooruEngine engine);
    // Relevance order for one prefix: exact match, then favourite tags,
    // then prefix matches by post count, then the rest (longer tags beat
    // short noise at equal counts). Pure: unit-tested. Applied to remote
    // answers (API order is count-only) before merging; local answers
    // arrive ranked already.
    static QVector<TagSuggestion> rank(const QString &prefix,
                                       QVector<TagSuggestion> v,
                                       const QSet<QString> &favTags = {});

signals:
    void finished(const QString &siteKey, const QString &prefix,
                  QVector<TagSuggestion> suggestions);

private:
    static QUrl urlFor(BooruEngine engine, const QString &baseUrl,
                       const QString &prefix, const SiteCreds &creds);
    class QNetworkAccessManager *m_nam;
    class QNetworkReply *m_reply = nullptr;
    QString m_siteKey; // site key of the in-flight request
    QString m_prefix;  // its prefix
    QSet<QString> m_favTags; // starred tags, lowercased (suggestion boost)
    QStringList m_historyTags; // recent search tokens, most-recent-first
    // Memory-only cache (cleared past a few hundred entries).
    QHash<QString, QVector<TagSuggestion>> m_cache;
    const Index *m_localIndex = nullptr;
    TagCountCache *m_tagCache = nullptr;
};
