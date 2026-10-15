#pragma once

// Favourites: database entries only, no downloads (LoliSnatcher-style).
// A favourite records enough to re-display the post later (grid + viewer).
// Lives in the data dir (store.db) - data, not cache.
#include <QObject>
#include <QString>
#include <QVector>

#include "post.h"

class FavDb {
public:
    FavDb() = default;
    ~FavDb() { close(); }

    // dir empty = default data dir. Creates dirs/tables.
    // False -> favourites unavailable.
    bool open(const QString &dir = {});
    bool isOpen() const { return m_open; }
    void close();

    bool isFavourite(const QString &site, qint64 postId);
    // Remember where a favourite lives on disk (download/import target).
    // rowToPost serves it as a local post when the file exists.
    bool setLocalPath(const QString &site, qint64 postId, const QString &path);
    // Batch version in ONE transaction. Keys are (site, post_id) pairs.
    int setLocalPaths(const QHash<QPair<QString, qint64>, QString> &paths);
    // Returns false when already present.
    bool add(const Post &post);
    // Batch insert in ONE transaction (imports): one fsync instead of one
    // per row. Returns rows added (duplicates skipped).
    int addMany(const QVector<Post> &posts);
    bool remove(const QString &site, qint64 postId);
    int count();
    // Newest first, for the favourites tab.
    QVector<Post> list(int limit = 5000);
    // Favourites with no recorded download location (imports predate the
    // local_path column, or files arrived by other means).
    struct Unlinked {
        QString site;
        qint64 id = 0;
        QString fileUrl;
    };
    QVector<Unlinked> unlinked();
    // Persist re-categorized tag splits (classifier runs): one transaction,
    // keyed like setLocalPaths. Without this, re-listing the tab would
    // drop the buckets (the DB is the tab's backing store).
    struct TagSplit {
        QString artist, character, copyright, general, meta, untyped;
    };
    int setTagSplits(const QHash<QPair<QString, qint64>, TagSplit> &splits);
    void clear();

private:
    bool m_open = false;
    QString m_conn;
};
