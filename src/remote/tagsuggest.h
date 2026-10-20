#pragma once

// Tag autocomplete for browse tabs: per-engine tag APIs, debounced by the
// caller, one in-flight request at a time, results cached per site+prefix.
// Danbooru categories and Gelbooru/Moebooru types share danbooru numbering
// (0=general 1=artist 3=copyright 4=character 5=meta); Szurubooru categories
// map like post tags. Shimmie exposes no tag API: always empty.
#include <QObject>
#include <QHash>
#include <QString>
#include <QVector>

#include "remotesource.h" // BooruEngine/BooruSite/SiteCreds

struct TagSuggestion {
    QString tag;
    QString bucket = QStringLiteral("general"); // suggestion colors
    int count = 0;
};

class Index;

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

    static QVector<TagSuggestion> parse(BooruEngine engine, const QByteArray &body);
    // Merge policy for one prefix: local rows first (your vocabulary wins
    // ties), then remote extras, case-insensitive dedup, capped.
    static QVector<TagSuggestion> merge(const QVector<TagSuggestion> &local,
                                        const QVector<TagSuggestion> &remote,
                                        int cap = 12);
    // Relevance order for one prefix: exact match, then prefix matches by
    // post count, then the rest (longer tags beat short noise at equal
    // counts). Pure: unit-tested. Applied to remote answers (API order is
    // count-only) before merging; local answers arrive ranked already.
    static QVector<TagSuggestion> rank(const QString &prefix,
                                       QVector<TagSuggestion> v);

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
    // Memory-only cache (cleared past a few hundred entries).
    QHash<QString, QVector<TagSuggestion>> m_cache;
    const Index *m_localIndex = nullptr;
};
