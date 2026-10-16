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

    // Row identity, shared by add/addMany and the UI's favIdentity:
    // site-labelled rows (remote AND downloaded) key by site+id, except
    // id-less imports (stray links all carry id 0) which key by a stable
    // content hash of the URL instead. Pure-local rows key by stable hash
    // of the file path. Stable (SHA1-based, NOT qHash: Qt does not
    // guarantee qHash stability across versions) so identities survive
    // restarts and upgrades.
    static qint64 pidFor(const Post &post);
    static QString siteFor(const Post &post) {
        if (post.siteName.isEmpty() && !post.isRemote)
            return QStringLiteral("local");
        return post.siteName;
    }

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
        QString tags; // combined tag blob (rebuilds sidecars on regen)
        QString ext;
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
    // One-time healing of hash-based identities (see pidFor): rewrites
    // unstable (site,0)/qHash rows to stable ids, collapsing duplicates.
    void migrateHashIds();

    bool m_open = false;
    QString m_conn;
};
