#pragma once

// Remote booru client: registry, async search, credential probing and
// single-post refresh. Per-engine request/response shapes live behind
// BooruEngineOps (booruengine.h: engine_danbooru.cpp and friends) and
// per-host quirks in sitequirks.h; this file owns the QNAM plumbing.
// The includes below keep this header a drop-in umbrella: existing
// users of engine/quirks helpers keep compiling unchanged.
#include <QNetworkProxy>
#include <QObject>

#include <functional>

#include "boorutypes.h"
#include "booruengine.h"
#include "sitequirks.h"

class RemoteSource : public QObject {    Q_OBJECT
public:
    // Shipped presets for brand-new users (Safebooru only: SFW by
    // default; anything else is one Add away in the Booru manager).
    // User-configured boorus live in AppSettings::boorus and are pushed
    // here at startup / prefs apply.
    static QList<BooruSite> presetBoorus();
    // Look up by key: configured boorus first, then presets.
    static const BooruSite *siteFor(const QString &key);
    // Full label -> key map (presets first, configured entries win).
    // Used to resolve download-folder names back to sites.
    static QMap<QString, QString> labelKeys();
    static void setConfiguredBoorus(const QList<BooruSite> &boorus);

    explicit RemoteSource(const BooruSite &site, QObject *parent = nullptr);
    void setCreds(const SiteCreds &creds) { m_creds = creds; }
    // Self-hosted engines (Szurubooru, Shimmie): override the empty base URL.
    void setBaseUrl(const QString &url) { m_baseUrl = url; }
    const QString &siteKey() const { return m_site.key; }
    const QVector<Post> &lastPosts() const { return m_lastPosts; }
    const QString &lastError() const { return m_lastError; }
    int lastHttp() const { return m_lastHttp; }
    const QByteArray &lastBody() const { return m_lastBody; }
    QUrl lastUrl() const { return m_lastUrl; }
    // Cookie NAMES (never values) on the last request: proves in the
    // Test viewer whether a clearance rode along at all.
    const QStringList &lastCookieNames() const { return m_lastCookieNames; }

    // Async page search. Tags use each API's native syntax (space-separated,
    // '-' exclusion works on all three families).
    void search(const QStringList &tags, int page, int limit);
    void cancel();

    // Synchronous credential check (limit=1 lookup, local event loop).
    static bool testCredentials(const BooruSite &site, const SiteCreds &creds,
                                QString &message);
    // Same check, but with the raw request/response attached for
    // user-visible diagnostics (click the Test result to inspect it).
    struct CredentialProbe {
        bool ok = false;
        QString message;
        QString siteLabel;
        int http = 0;
        QUrl url;
        QByteArray body; // raw response, capped
        QStringList sentCookieNames; // Cookie names on the probe request
    };
    static CredentialProbe probeCredentials(const BooruSite &site,
                                            const SiteCreds &creds);
    // Browser-backed fetch override for proof-of-work-walled hosts
    // (some walls bind clearance to the TLS fingerprint, so no harvested
    // cookie jar authorizes plain requests). Set once by the app to a
    // webview-backed fetcher; empty by default (fallback disabled, e.g.
    // in tests). Input: API URL + timeout ms.
    struct WebFetchResult {
        int http = 0;
        QByteArray body;
        QString error;
    };
    using WebFetchFn = std::function<WebFetchResult(const QUrl &, int)>;
    static void setWebFetcher(WebFetchFn fn);
    static WebFetchFn webFetcher(); // empty when unset
    // Attach the matching configured site's cookies to req (host match,
    // subdomains included, e.g. media CDNs). One line at every request
    // site; silently skips when nothing matches. Covers captcha and
    // Cloudflare-walled instances for API + media alike.
    // Returns true when a Cookie header was attached.
    static bool applyCookies(class QNetworkRequest &req);

signals:
    void searchFinished(const SearchResult &result);

private:
    QUrl buildUrl(const QStringList &tags, int page, int limit) const;
    SearchResult parse(const QByteArray &body) const;
    // Browser fallback for proof-of-work walls (see WebApiFetcher):
    // fetches m_lastUrl in the capture profile and emits the parsed
    // result. Emits only on success; otherwise fills note and returns
    // false so the normal wall error (plus note) is shown.
    bool tryWebRetry(QString &note);
    BooruEngineOps::Context opsContext() const {
        return {m_site.engine, m_baseUrl, m_creds};
    }

    BooruSite m_site;
    QString m_baseUrl; // == m_site.baseUrl except self-hosted engines
    SiteCreds m_creds;
    bool m_webRetried = false; // browser fallback already spent this search
    class QNetworkAccessManager *m_nam;
    class QNetworkReply *m_reply = nullptr;
    QVector<Post> m_lastPosts;
    QString m_lastError;
    int m_lastHttp = 0;
    QByteArray m_lastBody; // raw body of the last search, capped
    QUrl m_lastUrl;        // exact request URL of the last search
    QStringList m_lastCookieNames; // Cookie names on the last request
    int m_pendingPage = 1;
    int m_pendingLimit = 40;
};

Q_DECLARE_METATYPE(RemoteSource::CredentialProbe)

namespace RemotePosts {
// Registry-dependent lookups (need RemoteSource::siteFor).
QString keyForLabel(const QString &siteLabel); // "Danbooru" -> "danbooru"
                                               // (custom labels via setConfiguredBoorus)
// Referer required by hotlink-protected media hosts (Gelbooru-family
// CDNs 302 to hotlink.php without one). Empty when unneeded.
QString mediaReferer(const QString &siteKey);
// Same, but derived from the post itself: the post page is the ideal
// referer (any same-host page passes the check) and needs no registry
// lookup, so renamed/custom/unknown site labels all work. Falls back to
// the configured base URL when the post carries no page link.
QString mediaRefererFor(const Post &post);
// Engine for a configured/preset key (Danbooru fallback when unknown).
// Lets refresh/notes/tag-refresh switches work for custom instances too.
BooruEngine engineForSiteKey(const QString &key);
// True when the post's entry opts out of automatic tag categorization
// (meme hosts: tags stay as they arrive). Local posts never skip.
bool skipAutoClassify(const Post &post);
// Effective bucket for one verdict/tag on this post: "general" when the
// bucket is disabled for the post's entry (per-host set) or its engine
// family (legacy grid). Identity otherwise ("general" never folds).
QString foldedBucketFor(const Post &post, const QString &bucket);
// The user agent of the cookie-capture browser ONLY (never sent by API,
// media or metadata requests — those identify honestly, since a browser
// UA over QNAM's OpenSSL stack is exactly the mismatch walls flag).
// The browser reports the engine's real default UA (see the setter);
// requests carry harvested clearance in the Cookie header instead.
QString captureUserAgent();
void setCaptureUserAgent(const QString &ua);
// Proxy for one request URL: the matching configured entry's proxy
// spec (empty = direct). Host match mirrors applyCookies (subdomains
// included).
QString proxyForUrl(const QString &url);
// Parse a proxy spec (http://[user:pass@]host:port, socks5://...).
// Default-constructed (direct) when empty or unparseable.
QNetworkProxy proxyFromSpec(const QString &spec);
// Redact credential values from a request URL for display
// (api_key=SECRET -> api_key=<redacted>). The request itself is untouched.
QString redactSecrets(const QString &url);
// Render one request/response block for the inspector dialog.
QString probeReportText(const QString &url, int http, const QByteArray &body);
// Modal read-only inspector for a (possibly multi-section) report.
void showProbeDialog(QWidget *parent, const QString &title, const QString &text);
} // namespace RemotePosts

// Refresh target: enough to re-fetch one post's API object.
struct RefreshTarget {
    QString siteKey; // danbooru|gelbooru|safebooru|yandere|konachan|szurubooru
    QString baseUrl;
    qint64 id = 0;
    SiteCreds creds;
    QStringList cookies; // entry's challenge-bypass cookies, like search
    QString proxy;       // entry's proxy spec, empty = direct
    bool valid() const { return !siteKey.isEmpty() && !baseUrl.isEmpty() && id > 0; }
};

// Resolve what (if anything) a post can refresh from (registry-aware):
//   remote posts -> their own site+id (empty base URL = not configured)
//   local LOSN posts -> host+serverId from the sidecar's postURL
//   anything else -> invalid (button disabled with a reason)
RefreshTarget resolveRefreshTarget(const Post &post);

// Single-post re-fetch (tag refresh). Emits the raw API object.
class PostRefresher : public QObject {
    Q_OBJECT
public:
    explicit PostRefresher(QObject *parent = nullptr);
    // reqId echoes in the signals so parallel flights (import) can tell
    // responses apart; single-flight callers ignore it.
    void refresh(const RefreshTarget &target, qint64 reqId = 0);
    void cancel();
    static QJsonObject parseSingle(BooruEngine engine, const QByteArray &body,
                                   bool &ok);

signals:
    void refreshed(qint64 reqId, const QJsonObject &raw);
    void failed(qint64 reqId, const QString &error);

private:
    QNetworkAccessManager *namForProxy(const QString &proxySpec);

    class QNetworkAccessManager *m_nam;
    class QNetworkReply *m_reply = nullptr;
    QMap<QString, class QNetworkAccessManager *> m_proxyNams;
    RefreshTarget m_target;
    qint64 m_reqId = 0;
};
