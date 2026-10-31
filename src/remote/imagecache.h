#pragma once

// Full-quality image cache for remote browsing (stills + GIFs; video
// streams are never cached). The viewer loads through here so posts open
// instantly on revisit and work offline.
//
// Two modes, one code path:
//   enabled  -> files persist under baseDir/<sha1(url)>.<ext>
//   disabled -> bytes stay in a bounded in-memory cache for the session
//               (and are staged in a session temp dir only because the
//               Qt readers need a path); destroyed on unload, never
//               written under baseDir
// Explicit Download (DownloadManager) is the ONLY thing that ever writes to
// the user's download folder, always at full quality.
#include <QByteArray>
#include <QHash>
#include <QSet>
#include <QMap>
#include <QObject>
#include <QString>

#include "post.h"

class QNetworkAccessManager;
class QNetworkReply;

class ImageCache : public QObject {
    Q_OBJECT
public:
    explicit ImageCache(QObject *parent = nullptr);
    ~ImageCache() override;

    void setEnabled(bool on);
    void setBaseDir(const QString &dir);
    // Existing cached file, or "" (caller should request()). Passing the
    // post applies the cache policy: denied entries behave as uncached
    // and are not read back from disk.
    QString localPath(const QString &url, const Post *post = nullptr) const;
    // Media policy is applied per post (global cache mode narrowed by
    // the entry's own override). A null post keeps the global mode.
    void request(const QString &url, const QString &referer = {},
                 const Post *post = nullptr);
    void cancelUrl(const QString &url);
    // Warm an adjacent video without caching it: a Range GET for the
    // first few MB (player start span), then abort. No disk/RAM entry.
    void prefetchRange(const QString &url, const QString &referer = {},
                       qint64 maxBytes = 2 * 1024 * 1024);
    // Forget one entry (delete the cached file): the next request()
    // refetches bytes. Used by viewer reload, not by normal browsing.
    void drop(const QString &url);

signals:
    void ready(const QString &url, const QString &localPath);
    void failed(const QString &url, const QString &error);
    // Live byte progress for an in-flight request (total <= 0 unknown).
    void downloadProgress(const QString &url, qint64 received, qint64 total);

private:
    QString targetPath(const QString &url, bool persist) const;
    static QString extOfUrl(const QString &url);
    // Session RAM cache (non-persistent mode): no network on revisit and
    // no durable files. Materialised to m_sessionTmpDir for Qt readers.
    QString ramPath(const QString &url) const;
    QString materializeRam(const QString &url) const;
    void storeRam(const QString &url, const QByteArray &bytes);
    static constexpr qint64 kRamLimit = 512LL * 1024 * 1024;
    static constexpr qint64 kRamEntryLimit = 64LL * 1024 * 1024;
    QNetworkAccessManager *namForProxy(const QString &proxySpec);

    QNetworkAccessManager *m_nam;
    QHash<QString, QNetworkReply *> m_active;
    QSet<QString> m_prefetched; // range warm-ups already issued
    QMap<QString, QNetworkAccessManager *> m_proxyNams;
    QString m_baseDir;
    QString m_sessionTmpDir;
    QHash<QString, QByteArray> m_ram;
    QStringList m_ramOrder; // oldest first
    qint64 m_ramBytes = 0;
    bool m_enabled = true;
};
