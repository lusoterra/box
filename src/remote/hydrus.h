#pragma once

// Read-only Hydrus tag lookup. Hydrus (and the standalone `hydd` Public
// Tag Repository mirror) serves file metadata by content hash:
//
//   GET <base>/get_files/file_metadata?hash=<hex>
//     header Hydrus-Client-API-Access-Key: <key>
//
// Hydrus stores files by SHA-256; old databases/mirrors still keep a tiny
// SHA-1/MD5 index for pre-2019 imports, so lookups may also be issued with
// a 40- or 32-character hash. Nothing here ever writes to Hydrus: the app
// only reads tags for stray editing (and a read-only viewer for any post).
#include <QByteArray>
#include <QHash>
#include <QNetworkProxy>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>

#include "filehash.h"

class QNetworkAccessManager;
class QNetworkReply;

namespace Hydrus {

enum class HashKind { Sha256, Sha1, Md5 };

QString hashKindLabel(HashKind kind); // "SHA-256" / "SHA-1" / "MD5"
QString hashKindKey(HashKind kind);   // "sha256" / "sha1" / "md5"
QString hashFor(HashKind kind, const FileContentHashes &hashes);

struct Config {
    bool enabled = false;
    QString baseUrl = QStringLiteral("http://127.0.0.1:45869");
    QString accessKey;
    QNetworkProxy proxy; // direct when unset
};

// One metadata row, reduced to what the app cares about.
struct FileTags {
    bool ok = false;    // the HTTP body parsed
    bool found = false; // the hash exists in the server
    QString hash;       // hash echoed by the server (wanted when absent)
    qint64 fileId = 0;
    QStringList tags; // deduped, alphabetical
    QString error;    // parse/semantic error when !ok
};

// Parse a /get_files/file_metadata response. `wantedHash` is used when the
// server omits the hash and to prefer the matching row in batched replies.
// A row whose file_id is null (or 0) is a miss, not an error.
FileTags parseMetadata(const QByteArray &body, const QString &wantedHash);

// Hydrus namespaces (`creator:`, `character:`, `series:`, …) folded into
// the app's five buckets. The prefix is stripped for mapped tags; anything
// else (including Hydrus-only namespaces such as `species:`) stays intact
// in General so no tag is lost.
struct CategorizedTags {
    QStringList artist;
    QStringList character;
    QStringList copyright;
    QStringList general;
    QStringList meta;
    // First rating:<x> tag seen ("safe", "explicit", …). Kept in General too.
    QString rating;
};
CategorizedTags categorize(const QStringList &tags);

// /verify_access_key summary, e.g. "hydd (Hydrus 696)".
QString verifySummary(const QByteArray &body);

// Build the endpoint URL for a hash lookup (pure; unit-tested).
QUrl metadataUrl(const QString &baseUrl, const QString &hash);

// hydd extension: exact network-wide mapping counts (`/hyd/tag_counts`).
// The official Hydrus client and the PTR have no equivalent; a server
// without the extension answers 404/501 and the caller keeps all tags.
struct TagCounts {
    bool ok = false;
    QHash<QString, int> counts; // normalized lowercase tag -> count
    QStringList unknown;       // tags hydd reports as undefined (normalized)
    QString error;
};
TagCounts parseTagCounts(const QByteArray &body);
QUrl tagCountsUrl(const QString &baseUrl, const QStringList &tags);

// Network client. One request at a time; a new lookup cancels the old one.
class Client : public QObject {
    Q_OBJECT
public:
    explicit Client(const Config &config, QObject *parent = nullptr);

    void verify();
    void lookup(const QString &hashHex, HashKind kind);
    // Requires hydd with tag_counts enabled. On a 404/501 (server without
    // the extension) or any transient failure, tagCountsFailed carries the
    // HTTP status so the caller can fall back to keeping every tag.
    void tagCounts(const QStringList &tags);
    void cancel();

signals:
    void found(const Hydrus::FileTags &file, Hydrus::HashKind kind);
    void notFound(Hydrus::HashKind kind);
    void error(const QString &message);
    void verified(bool ok, const QString &message);
    void tagCountsReady(const QHash<QString, int> &counts,
                        const QStringList &unknown);
    void tagCountsFailed(const QString &message, int httpStatus);

private:
    enum class Request { Metadata, Verify, TagCounts };
    void send(const QUrl &url);
    QNetworkAccessManager *m_nam = nullptr;
    QNetworkReply *m_reply = nullptr;
    Config m_config;
    HashKind m_kind = HashKind::Sha256;
    QString m_hash;
    Request m_request = Request::Metadata;
};

} // namespace Hydrus

Q_DECLARE_METATYPE(Hydrus::FileTags)
Q_DECLARE_METATYPE(Hydrus::HashKind)
