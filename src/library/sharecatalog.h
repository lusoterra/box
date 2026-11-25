#pragma once

// Read-only catalog behind the LAN sharing server. It holds a snapshot of
// posts from any mix of sources (Hydrus tab, downloads, favourites, a local
// archive) and emits Danbooru-shaped JSON. Duplicate rows from different
// sources collapse on site+id, content hash, post page or canonical path.
#include "post.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

struct ShareItem {
    qint64 id = 0;          // stable numeric id exposed as `id`
    Post post;
    QString localPath;      // served directly when it exists
    QString remoteMediaUrl; // remote-only favourites: proxy/cache target
    QString remotePreviewUrl;
};

class ShareCatalog {
public:
    // Adds or merges one post. localPath may be empty for remote-only rows;
    // remoteMediaUrl is then used as the media target. Rows with neither a
    // local file nor a media URL are skipped (the API needs a file_url).
    bool add(const Post &post, const QString &localPath = {},
             const QString &remoteMediaUrl = {});
    void clear();

    int size() const { return static_cast<int>(m_items.size()); }
    const QVector<ShareItem> &items() const { return m_items; }
    const ShareItem *byId(qint64 id) const;

    // Danbooru-shaped pages. `tags` follows the app/engine syntax (AND
    // terms, -exclusions, ~(...) groups, rating:/filetype:/id:/md5:).
    QVector<const ShareItem *> search(const QString &tags, int page, int limit,
                                      int *total = nullptr) const;
    QJsonArray postsJson(const QVector<const ShareItem *> &items,
                         const QString &baseUrl) const;
    QJsonObject postJson(const ShareItem &item, const QString &baseUrl) const;
    QJsonArray tagsJson(const QString &prefix, int limit) const;
    int count(const QString &tags) const;

    static qint64 stableId(const QString &key);
    static QString previewUrlFor(const ShareItem &item,
                                 const QString &baseUrl);

private:
    QStringList keysFor(const Post &post, const QString &localPath,
                        const QString &remoteMediaUrl) const;
    void mergeInto(ShareItem &dst, const Post &post, const QString &localPath,
                   const QString &remoteMediaUrl);
    void registerKeys(int index, const QStringList &keys);

    QVector<ShareItem> m_items;
    QHash<QString, int> m_byKey;
    QHash<qint64, int> m_byId;
};
