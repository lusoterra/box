#pragma once

// Tag-category classification for untyped tags.
//
// Problem: LOSN/Gelbooru sidecars carry tags with no category
// (tagType "none" / one flat blob), so everything lands in General.
// Fix: look the category up and re-bucket, persistently.
//
// Sources (danbooru numbering
// 0=general 1=artist 3=copyright 4=character 5=meta 6=deprecated):
//   1. Gelbooru tag API when credentials are set (exact `name=` match, XML;
//      anonymous requests 401, so this stage is skipped without creds)
//   2. Danbooru tag API, anonymous (same tag universe + numbering, JSON)
//   3. Safebooru tag API fallback (SFW-filtered, misses more)
//   4. yandere tag API fallback (exact-name pick from substring results, JSON)
// Results - including misses (type -1) - persist in the per-root index DB's
// tag_types table, so each tag is fetched once ever. Transient network
// failures are NOT cached and retried on the next scan.
#include <QHash>
#include <QList>
#include <QMap>
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
int parseDanbooruTagsJson(const QByteArray &data, const QString &wanted); // -2 on error
constexpr int kTransientFailure = -2;
constexpr int kUnknownTag = -1;

// Manual per-tag overrides (Preferences → Tag type overrides): tags the
// online classifier missed or got wrong. Keys are lowercase, values are
// bucketForType() names ("general" pins a tag in General).
inline bool isValidBucket(const QString &bucket) {
    return bucket == QStringLiteral("artist") ||
           bucket == QStringLiteral("copyright") ||
           bucket == QStringLiteral("character") ||
           bucket == QStringLiteral("meta") || bucket == QStringLiteral("general");
}

// Parse "tag=bucket" lines (settings.ini form): lowercase, trimmed,
// validated; later lines win; garbage dropped.
inline QMap<QString, QString> normalizeTagOverrides(const QStringList &lines) {
    QMap<QString, QString> out;
    for (const QString &line : lines) {
        const int eq = line.indexOf(u'=');
        if (eq <= 0)
            continue;
        const QString tag = line.left(eq).trimmed().toLower();
        const QString bucket = line.mid(eq + 1).trimmed().toLower();
        if (tag.isEmpty() || !isValidBucket(bucket))
            continue;
        out.insert(tag, bucket);
    }
    return out;
}

inline QStringList serializeTagOverrides(const QMap<QString, QString> &overrides) {
    QStringList out;
    for (auto it = overrides.begin(); it != overrides.end(); ++it)
        out << it.key() + u'=' + it.value();
    return out;
}

// Seed-file form (`tag<TAB>category` per line, scripts/seed_tagtypes.py):
// lowercase tag, danbooru category int. Blanks, `#` comments and malformed
// lines are dropped; later lines win.
QHash<QString, int> parseTagSeed(const QByteArray &tsv);
} // namespace TagTypes

// Persistent tag->type store. Owns one QSql connection; create, use and
// destroy on a single thread (scanner worker OR gui, never shared).
class TagTypeCache {
public:
    TagTypeCache() = default;
    ~TagTypeCache() { close(); }

    bool open(const QString &dbPath);
    bool isOpen() const { return m_open; }
    // Batch a whole fetch run into ONE transaction (hundreds of per-row
    // fsyncs on the GUI thread otherwise). Commit even partial runs.
    bool beginBatch();
    bool endBatch(bool commit);
    // Bulk-load a maintainer seed map (INSERT OR IGNORE: rows the user
    // already fetched keep their stored verdicts). Returns rows added.
    int importSeed(const QHash<QString, int> &types);
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
// Gelbooru first when credentials exist, else anonymous Danbooru, then
// Safebooru and yandere as fallbacks. Emits progress + one finished map.
class TagTypeFetcher : public QObject {
    Q_OBJECT
public:
    explicit TagTypeFetcher(QObject *parent = nullptr);

    // Optional Gelbooru credentials (user ID + API key, sent together or
    // not at all). Gelbooru's tag API 401s anonymously, so without them
    // the anonymous Danbooru index is used instead.
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
    // Streaming verdicts: one small map per resolved tag, emitted as the
    // run goes so the sidebar can fill in like a waterfall instead of
    // waiting for finished. finished() still carries the full map.
    void resolved(const QHash<QString, QString> &buckets);
    void validated(bool ok, const QString &message);
    void gelbooruAuthFailed();
    void backoff(); // rate-limited: launches paused briefly

private:
    enum class Source { Gelbooru, Danbooru, Safebooru, Yandere };
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
    QNetworkAccessManager *namForProxy(const QString &proxySpec);

    QNetworkAccessManager *m_nam;
    QTimer *m_backoff;
    QList<QString> m_queue;
    QList<Flight *> m_flights;
    QList<Source> m_order;
    QHash<QString, QString> m_results;
    TagTypeCache m_cache;
    QMap<QString, QNetworkAccessManager *> m_proxyNams;
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
