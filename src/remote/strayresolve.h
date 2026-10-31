#pragma once

// Stray link resolution. "Download/Add Stray" accepts anything the user
// finds on the internet: a direct image/video URL or a post page on a
// booru that is not (and will not become) a configured entry. Recognised
// post pages are read through their public API first, then the post page
// HTML, so the media download also gets a full JSON sidecar (tags,
// buckets, post page) instead of an untagged blob.
#include <QMap>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include "booruengine.h"
#include "post.h"

class QNetworkAccessManager;
class QNetworkReply;

namespace RemotePosts {
// Post-page recognition (pure, unit-tested). baseUrl keeps a sub-path
// install prefix; engines are the API families to probe, in order.
struct BooruPostUrl {
    qint64 id = 0;
    QString baseUrl;
    QVector<BooruEngine> engines;
    bool valid() const {
        return id > 0 && !baseUrl.isEmpty() && !engines.isEmpty();
    }
};
// Empty/invalid when the URL is not a recognisable booru post page (direct
// media and arbitrary pages simply pass through as-is).
BooruPostUrl parseBooruPostUrl(const QString &url);

// Post-page HTML -> Post (og:image + li.tag-type-* / data-* attributes).
// Used when the API refuses anonymous requests. Never invents a media URL:
// fileUrl stays empty when the page has no usable image meta.
Post parseBooruPageHtml(const QByteArray &body, const QString &pageUrl,
                        qint64 fallbackId);

// Map one raw API object through its engine's mapper, with a generic
// fallback for public shapes outside our engine families (e621-style
// file.url/tags-object posts).
Post postFromRaw(BooruEngine engine, const QJsonObject &raw,
                 const QString &baseUrl);
} // namespace RemotePosts

// Resolves pasted links in order, one network flight at a time.
class StrayResolver : public QObject {
    Q_OBJECT
public:
    struct Result {
        QString sourceUrl;
        Post post; // isRemote + siteName "_Stray" on success
        bool booru = false; // true: resolved from a post page API/HTML
        bool ok = false;
        QString error;
    };

    explicit StrayResolver(QObject *parent = nullptr);
    // Trims/filters blank lines; non-http(s) inputs become error results.
    void resolve(const QStringList &urls);
    void cancel();

signals:
    void progress(int done, int total, const QString &url);
    void finished(const QVector<StrayResolver::Result> &results);

private:
    void startNext();
    void requestApi();
    void tryHtmlFallback();
    void acceptRaw(const QJsonObject &raw);
    void acceptPost(Post post, bool booru);
    void failCurrent(const QString &error);
    void doneCurrent();

    QNetworkAccessManager *namForProxy(const QString &proxySpec);
    QString proxySpec() const;

    QStringList m_urls;
    QVector<Result> m_results;
    int m_index = 0;
    RemotePosts::BooruPostUrl m_match;
    int m_candidate = 0;
    BooruEngine m_pendingEngine = BooruEngine::Danbooru;
    bool m_htmlTried = false;
    bool m_cancelled = false;
    class QNetworkReply *m_reply = nullptr;
    class QNetworkAccessManager *m_nam = nullptr;
    QMap<QString, class QNetworkAccessManager *> m_proxyNams;
};

Q_DECLARE_METATYPE(StrayResolver::Result)
