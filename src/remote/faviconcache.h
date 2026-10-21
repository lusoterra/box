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
#include <QRegularExpression>
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

// Icon URLs advertised by page HTML (<link rel="icon" / "shortcut icon" /
// "apple-touch-icon" href=...>), resolved against pageUrl. Exact "icon"
// rels first (document order within each tier), then touch/mask icons;
// data:/javascript: hrefs and dupes dropped, http(s) only. Pure
// (unit-tested): the fetcher runs it on HTML bodies where an image was
// expected (404/landing pages) and queues the results before giving up.
inline QStringList faviconIconsFromHtml(const QByteArray &html,
                                        const QString &pageUrl) {
    if (html.isEmpty() || html.size() > 1048576 || pageUrl.trimmed().isEmpty())
        return {};
    const QUrl base(pageUrl);
    if (!base.isValid() || base.host().isEmpty())
        return {};
    const QString text = QString::fromUtf8(html);
    static const QRegularExpression tagRe(QStringLiteral("<link\\b[^>]*>"),
                                          QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression attrRe(
        QStringLiteral("(rel|href)\\s*=\\s*(?:\"([^\"]*)\"|'([^']*)'|([^\\s>]+))"),
        QRegularExpression::CaseInsensitiveOption);
    struct Hit {
        QString url;
        bool exact;
    };
    QList<Hit> hits;
    auto tags = tagRe.globalMatch(text);
    while (tags.hasNext()) {
        const QString tag = tags.next().captured(0);
        QString rel, href;
        auto attrs = attrRe.globalMatch(tag);
        while (attrs.hasNext()) {
            const auto m = attrs.next();
            const QString name = m.captured(1).toLower();
            QString v = !m.captured(2).isNull()   ? m.captured(2)
                        : !m.captured(3).isNull() ? m.captured(3)
                                                  : m.captured(4);
            if (name == QStringLiteral("rel"))
                rel = v;
            else
                href = v;
        }
        if (rel.isEmpty() || href.isEmpty())
            continue;
        bool isIcon = false, exact = false;
        for (const QString &tok :
             rel.toLower().split(u' ', Qt::SkipEmptyParts)) {
            if (tok.contains(QStringLiteral("icon"))) {
                isIcon = true;
                if (tok == QStringLiteral("icon"))
                    exact = true;
            }
        }
        if (!isIcon)
            continue;
        const QString h = href.trimmed();
        if (h.isEmpty() || h.startsWith(u'#') ||
            h.startsWith(QStringLiteral("data:"),
                         Qt::CaseInsensitive) ||
            h.startsWith(QStringLiteral("javascript:"),
                         Qt::CaseInsensitive))
            continue;
        const QUrl resolved = base.resolved(QUrl(h));
        const QString scheme = resolved.scheme().toLower();
        if ((scheme == QStringLiteral("http") ||
             scheme == QStringLiteral("https")) &&
            !resolved.host().isEmpty())
            hits.append({resolved.toString(), exact});
    }
    QStringList out;
    for (int pass = 0; pass < 2; ++pass) {
        for (const Hit &h : hits) {
            if ((pass == 0) != h.exact)
                continue;
            if (!out.contains(h.url))
                out << h.url;
            if (out.size() >= 8)
                return out;
        }
    }
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
    QSet<QString> m_triedUrls; // fetched this round: HTML re-parses must not loop
};
