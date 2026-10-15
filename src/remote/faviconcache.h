#pragma once

// Per-site favicons for tabs and menus (LoliSnatcher-style site icons).
// Fetched once per site from <base>/favicon.ico (falling back to the host
// root for subpath instances like example.com/booru), converted to PNG and
// cached under <cacheDir>/favicons/<key>.png. Lookups are synchronous from
// memory/disk; misses fetch in the background and emit ready().
#include <QHash>
#include <QIcon>
#include <QImage>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QUrl>

// Candidate favicon URLs, most specific first. Pure (unit-tested).
// Subpath bases try their own path first, then the host root.
inline QStringList faviconCandidates(const QString &baseUrl) {
    QString base = baseUrl.trimmed();
    while (base.endsWith(u'/'))
        base.chop(1);
    if (base.isEmpty())
        return {};
    QStringList out;
    out << base + QStringLiteral("/favicon.ico");
    const QString path = QUrl(base).path();
    if (!path.isEmpty() && path != QStringLiteral("/")) {
        const QUrl root(base);
        out << root.scheme() + QStringLiteral("://") + root.host() +
                   QStringLiteral("/favicon.ico");
    }
    out.removeDuplicates();
    return out;
}

class FaviconCache : public QObject {
    Q_OBJECT
public:
    explicit FaviconCache(const QString &cacheDir, QObject *parent = nullptr);

    // Cached icon, or empty (queues a background fetch on first miss).
    QIcon iconFor(const QString &key, const QString &baseUrl);
    void prefetch(const QString &key, const QString &baseUrl);
    // Custom icon override dir (<dataDir>/favicons, set by the app):
    // <dir>/<key>.png wins over everything when present (Booru manager
    // "Choose…" for sites whose favicon refuses to fetch). Never fetched
    // into, never cleared by refreshAll (user data, not cache).
    // Auto-fetched icons live in <dir>/auto/ for the same reason: the
    // data dir is what users back up, the cache dir is disposable.
    void setCustomDir(const QString &dir);
    QString customPathFor(const QString &key) const;
    QString autoPathFor(const QString &key) const;
    void clearPending(); // drop queued/in-flight fetches (registry changed)
    // Drop everything (memory + disk) so icons refetch fresh. Callers
    // re-prefetch afterwards; arrivals come back through ready().
    void refreshAll();

signals:
    void ready(const QString &key, QIcon icon);

private:
    void fetchNext();
    void finishFetch();

    QString m_dir;
    QString m_customDir;
    QHash<QString, QIcon> m_icons;   // key -> icon (incl. fallbacks below)
    QHash<QString, QString> m_files; // key -> cached png path
    QSet<QString> m_dead;            // keys with no usable favicon (session)
    QList<QPair<QString, QString>> m_queue; // (key, baseUrl) FIFO
    QSet<QString> m_queued;
    class QNetworkAccessManager *m_nam;
    class QNetworkReply *m_reply = nullptr;
    QString m_currentKey;
    QStringList m_currentCandidates;
};
