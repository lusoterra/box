#pragma once

// Tag-category classification for untyped tags.
//
// Problem: LOSN/Gelbooru sidecars carry tags with no category
// (tagType "none" / one flat blob), so everything lands in General.
// Fix: look the category up and re-bucket, persistently.
//
// Sources (both public, no API key, danbooru numbering
// 0=general 1=artist 3=copyright 4=character 5=meta 6=deprecated):
//   1. Safebooru tag API (exact `name=` match, XML)
//   2. yandere tag API fallback (exact-name pick from substring results, JSON)
// Results — including misses (type -1) — persist in the per-root index DB's
// tag_types table, so each tag is fetched once ever. Transient network
// failures are NOT cached and retried on the next scan.
#include <QHash>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>

class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

namespace TagTypes {
QString bucketForType(int type); // "artist"|"copyright"|"character"|"meta"|"general"
int typeForBucket(const QString &bucket);

// Response parsers (pure, unit-tested). `wanted` must match exactly.
// The Gelbooru-family tag XML schema is shared by Gelbooru and Safebooru.
int parseSafebooruXml(const QByteArray &data, const QString &wanted); // -2 on error
int parseYandereJson(const QByteArray &data, const QString &wanted);  // -2 on error
constexpr int kTransientFailure = -2;
constexpr int kUnknownTag = -1;
} // namespace TagTypes

// Persistent tag->type store. Owns one QSql connection; create, use and
// destroy on a single thread (scanner worker OR gui, never shared).
class TagTypeCache {
public:
    TagTypeCache() = default;
    ~TagTypeCache() { close(); }

    bool open(const QString &dbPath);
    bool isOpen() const { return m_open; }
    // True when the tag has a stored verdict (type may be kUnknownTag).
    bool get(const QString &tag, int &type);
    void put(const QString &tag, int type);
    void putBatch(const QHash<QString, int> &types);
    void close();

private:
    bool m_open = false;
    QString m_conn;
};

// Sequential-ish fetcher (GUI thread): up to 4 concurrent lookups,
// Safebooru first, yandere fallback. Emits progress + one finished map.
class TagTypeFetcher : public QObject {
    Q_OBJECT
public:
    explicit TagTypeFetcher(QObject *parent = nullptr);

    // Optional Gelbooru credentials (BOTH user ID and API key are required
    // by their API). When set, Gelbooru is queried first; Safebooru/yandere
    // remain as fallbacks for misses.
    void setGelbooruCredentials(const QString &userId, const QString &apiKey);

    void fetch(const QStringList &tags, const QString &dbPath);
    void cancel();
    bool isActive() const { return m_active; }
    int failures() const { return m_failures; }

    // One-shot credential check (known tag, reports what the API said).
    void validateCredentials(const QString &userId, const QString &apiKey);

signals:
    void progress(int done, int total);
    // tag -> bucket ("artist"|"copyright"|"character"|"meta"); misses omitted.
    void finished(const QHash<QString, QString> &buckets);
    void validated(bool ok, const QString &message);
    void gelbooruAuthFailed();
    void backoff(); // rate-limited: launches paused briefly

private:
    enum class Source { Gelbooru, Safebooru, Yandere };
    struct Flight {
        QString tag;
        QNetworkReply *reply = nullptr;
        int stage = 0; // index into m_order
        int attempts = 0; // 429 retries at the current stage
    };
    void pump();
    void issue(Flight *flight);
    void onReply(Flight *flight);
    static int parseFor(Source src, const QByteArray &body, const QString &tag);

    QNetworkAccessManager *m_nam;
    QTimer *m_backoff;
    QList<QString> m_queue;
    QList<Flight *> m_flights;
    QList<Source> m_order;
    QHash<QString, QString> m_results;
    TagTypeCache m_cache;
    QString m_gelbooruUser;
    QString m_gelbooruKey;
    bool m_gelbooruDead = false; // 401/403 this run: skip it for the rest
    int m_done = 0;
    int m_total = 0;
    int m_failures = 0; // transient (uncached) failures this run
    bool m_active = false;
    static constexpr int kMaxFlight = 4;
    static constexpr int kMaxAttempts = 3;
    static constexpr int kBackoffMs = 5000;
};
