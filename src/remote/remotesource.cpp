// RemoteSource client: site registry, async search, credential probing,
// single-post refresh and diagnostics. Per-engine request/response shapes
// live behind BooruEngineOps (engine_danbooru.cpp and friends); per-host
// quirks in sitequirks.cpp.
#include "remotesource.h"
#include "powsolver.h"

namespace {
// Browser-backed fetch override (see setWebFetcher). Empty = disabled.
RemoteSource::WebFetchFn &webFetcherSlot() {
    static RemoteSource::WebFetchFn fn;
    return fn;
}
} // namespace

void RemoteSource::setWebFetcher(WebFetchFn fn) {
    webFetcherSlot() = std::move(fn);
}

RemoteSource::WebFetchFn RemoteSource::webFetcher() {
    return webFetcherSlot();
}

#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QEventLoop>
#include <QFontDatabase>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPlainTextEdit>
#include <QRegularExpression>
#include <QTimer>
#include <QVBoxLayout>
#include <QUrl>

QList<BooruSite> RemoteSource::presetBoorus() {
    // New users start SFW-only: just Safebooru. Everything else is one
    // Add away in Preferences → Booru manager (existing configs keep
    // whatever they already have).
    BooruSite safe;
    safe.key = QStringLiteral("safebooru");
    safe.label = QStringLiteral("Safebooru");
    safe.shortName = QStringLiteral("SFW");
    safe.badgeColor = RemotePosts::defaultBadgeColor(
        QStringLiteral("https://safebooru.donmai.us"), BooruEngine::Danbooru);
    safe.baseUrl = QStringLiteral("https://safebooru.donmai.us");
    safe.engine = BooruEngine::Danbooru;
    return {safe};
}

namespace {
// User-configured boorus (pushed from AppSettings at startup / prefs
// apply). siteFor() prefers these; presets stay as fallback so old tabs
// and favourites keep resolving.
QList<BooruSite> &configuredBoorus() {
    static QList<BooruSite> configured;
    return configured;
}
QMap<QString, QString> &labelKeyMap() {
    static QMap<QString, QString> map;
    return map;
}
} // namespace

QMap<QString, QString> RemoteSource::labelKeys() {
    QMap<QString, QString> out;
    for (const BooruSite &b : presetBoorus())
        out.insert(b.label, b.key);
    for (auto it = labelKeyMap().begin(); it != labelKeyMap().end(); ++it)
        out.insert(it.key(), it.value());
    return out;
}

void RemoteSource::setConfiguredBoorus(const QList<BooruSite> &boorus) {
    configuredBoorus() = boorus;
    labelKeyMap().clear();
    for (const BooruSite &b : boorus)
        labelKeyMap().insert(b.label, b.key);
}

namespace RemotePosts {

namespace {
// Process-wide default proxy (Preferences -> Browse -> Global proxy).
// Read on every lookup so Apply takes effect without a restart.
QString &globalProxySlot() {
    static QString spec;
    return spec;
}
} // namespace

void setGlobalProxy(const QString &spec) {
    globalProxySlot() = spec.trimmed();
}

QString globalProxySpec() {
    return globalProxySlot();
}

QString proxyForUrl(const QString &url) {
    const QString host = QUrl(url).host().toLower();
    if (host.isEmpty())
        return {};
    for (const BooruSite &s : configuredBoorus()) {
        if (s.proxy.trimmed().isEmpty() || s.baseUrl.isEmpty())
            continue;
        const QString siteHost = QUrl(s.baseUrl).host().toLower();
        if (siteHost.isEmpty())
            continue;
        if (host == siteHost || host.endsWith(u'.' + siteHost))
            return s.proxy.trimmed();
    }
    return {};
}

QNetworkProxy proxyFromSpec(const QString &spec) {
    // Empty per-entry spec falls back to the global proxy, so every
    // consumer (site QNAMs included) honors one setting. Explicit
    // per-entry specs always win; "direct" opts one entry back out.
    QString use = spec.trimmed();
    if (use.isEmpty())
        use = globalProxySlot();
    if (use.compare(QStringLiteral("direct"), Qt::CaseInsensitive) == 0 ||
        use.compare(QStringLiteral("none"), Qt::CaseInsensitive) == 0)
        return QNetworkProxy();
    const QUrl url(use, QUrl::StrictMode);
    if (!url.isValid() || url.host().isEmpty() || url.port() <= 0)
        return QNetworkProxy();
    // NOTE: Socks5Proxy resolves the target hostname through the
    // proxy (Qt sends the domain name in the SOCKS handshake, like
    // curl --socks5-hostname); nothing leaks to local DNS. HttpProxy
    // likewise hands the proxy the hostname (absolute URI/CONNECT).
    QNetworkProxy::ProxyType type = QNetworkProxy::NoProxy;
    const QString scheme = url.scheme().toLower();
    if (scheme == QStringLiteral("http") || scheme == QStringLiteral("https"))
        type = QNetworkProxy::HttpProxy;
    else if (scheme == QStringLiteral("socks5") ||
             scheme == QStringLiteral("socks5h") ||
             scheme == QStringLiteral("socks"))
        type = QNetworkProxy::Socks5Proxy;
    else
        return QNetworkProxy();
    QNetworkProxy proxy(type, url.host(), quint16(url.port()),
                         url.userName(), url.password());
    return proxy;
}

bool proxyLooksLikeTor(const QString &spec) {
    const QNetworkProxy proxy = proxyFromSpec(spec);
    if (proxy.type() != QNetworkProxy::Socks5Proxy)
        return false;
    const QString host = proxy.hostName().toLower();
    if (host != QStringLiteral("127.0.0.1") &&
        host != QStringLiteral("localhost") && host != QStringLiteral("::1"))
        return false;
    return proxy.port() == 9050 || proxy.port() == 9150;
}

QString chromiumProxyServer(const QString &spec) {
    const QString use = spec.trimmed();
    if (use.isEmpty() || use.compare(QStringLiteral("direct"),
                                     Qt::CaseInsensitive) == 0 ||
        use.compare(QStringLiteral("none"), Qt::CaseInsensitive) == 0)
        return {};
    const QUrl url(use, QUrl::StrictMode);
    if (!url.isValid() || url.host().isEmpty() || url.port() <= 0)
        return {};
    // Chromium schemes: http/https/socks/socks4/socks5. socks5h is a box
    // spelling for remote-DNS SOCKS5, which is what Chromium's socks5 does.
    QString scheme = url.scheme().toLower();
    if (scheme == QStringLiteral("socks5h") ||
        scheme == QStringLiteral("socks"))
        scheme = QStringLiteral("socks5");
    if (scheme != QStringLiteral("http") &&
        scheme != QStringLiteral("https") &&
        scheme != QStringLiteral("socks5"))
        return {};
    return scheme + QStringLiteral("://") + url.authority();
}

void applyWebEngineProxy(const QString &globalProxy,
                         const QList<BooruSite> &boorus) {
    // Must run before the first QWebEngine* object (WebEngine reads the
    // flags once, at init). After that only a restart re-routes it.
    const QByteArray env = qgetenv("QTWEBENGINE_CHROMIUM_FLAGS");
    if (env.contains("--proxy-server"))
        return; // explicit user flag wins
    QString value = chromiumProxyServer(globalProxy);
    if (value.isEmpty()) {
        for (const BooruSite &b : boorus) {
            value = chromiumProxyServer(b.proxy);
            if (!value.isEmpty())
                break;
        }
    }
    if (value.isEmpty())
        return;
    QByteArray flags = env;
    if (!flags.trimmed().isEmpty())
        flags += ' ';
    flags += "--proxy-server=" + value.toLatin1();
    qputenv("QTWEBENGINE_CHROMIUM_FLAGS", flags);
}

} // namespace RemotePosts

const BooruSite *RemoteSource::siteFor(const QString &key) {
    for (const BooruSite &s : configuredBoorus()) {
        if (s.key == key)
            return &s;
    }
    static const QList<BooruSite> presets = presetBoorus();
    for (const BooruSite &s : presets) {
        if (s.key == key)
            return &s;
    }
    return nullptr;
}

RemoteSource::RemoteSource(const BooruSite &site, QObject *parent)
    : QObject(parent), m_site(site), m_baseUrl(site.baseUrl),
      m_nam(new QNetworkAccessManager(this)) {
    // Per-entry proxy (empty = direct, the historical behavior).
    const QNetworkProxy proxy = RemotePosts::proxyFromSpec(site.proxy);
    if (proxy.type() != QNetworkProxy::NoProxy)
        m_nam->setProxy(proxy);
}

bool RemoteSource::applyCookies(QNetworkRequest &req) {
    const QString host = req.url().host().toLower();
    if (host.isEmpty())
        return false;
    for (const BooruSite &s : configuredBoorus()) {
        if (s.cookies.isEmpty() || s.baseUrl.isEmpty())
            continue;
        const QString siteHost = QUrl(s.baseUrl).host().toLower();
        if (siteHost.isEmpty())
            continue;
        // Exact host or a subdomain of it (media CDNs live next door).
        if (host != siteHost && !host.endsWith(u'.' + siteHost))
            continue;
        const QString header = RemotePosts::cookieHeaderValue(s.cookies);
        if (header.isEmpty())
            return false;
        req.setRawHeader("Cookie", header.toUtf8());
        return true; // first matching site wins
    }
    return false;
}

namespace {
// The entry's own cookies ride its requests (exact host or subdomain,
// e.g. media CDNs). This runs AFTER the registry attach in timedRequest
// so dialog edits (capture → Test before Apply) win over last-saved
// settings; identical otherwise.
void attachSiteCookies(QNetworkRequest &req, const QString &baseUrl,
                       const QStringList &cookies) {
    if (cookies.isEmpty() || baseUrl.isEmpty())
        return;
    const QString host = req.url().host().toLower();
    const QString siteHost = QUrl(baseUrl).host().toLower();
    if (host.isEmpty() || siteHost.isEmpty())
        return;
    if (host != siteHost && !host.endsWith(u'.' + siteHost))
        return;
    const QString header = RemotePosts::cookieHeaderValue(cookies);
    if (!header.isEmpty())
        req.setRawHeader("Cookie", header.toUtf8());
}
QNetworkRequest timedRequest(const QUrl &url) {    QNetworkRequest req(url);
    req.setTransferTimeout(20000);
    // Clearance rides in the Cookie header; the UA stays honest. A
    // browser UA over QNAM's OpenSSL stack is exactly the mismatch
    // Cloudflare flags (measured: generic UA 200s, faked Chrome 403s).
    RemoteSource::applyCookies(req);
    req.setHeader(QNetworkRequest::UserAgentHeader,
                  QStringLiteral("box/0.1 (local booru browser)"));
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    return req;
}

} // namespace

namespace RemotePosts {

// Cloudflare "Just a moment" walls (403 interactive challenge, or 200
// HTML instead of data). The body is markup soup, so recognize the wall
// and say what to do instead of quoting it.
bool looksLikeCloudflareWall(const QByteArray &body) {
    if (body.isEmpty() || body.size() > 131072)
        return false;
    const QString text = QString::fromUtf8(body.left(32768));
    return text.contains(QStringLiteral("Just a moment"), Qt::CaseInsensitive) ||
           text.contains(QStringLiteral("_cf_chl_opt")) ||
           text.contains(QStringLiteral("challenges.cloudflare.com")) ||
           text.contains(QStringLiteral("challenge-platform")) ||
           text.contains(QStringLiteral("processChallenge"));
}
// JS proof-of-work / captcha walls answer HTTP 200 with an HTML page
// instead of data (ATF's "Verification" page, Cloudflare challenge
// leftovers). Recognize those bodies so the error names the wall.
bool looksLikeChallengePage(const QByteArray &body) {
    if (body.isEmpty() || body.size() > 65536)
        return false;
    const QString text = QString::fromUtf8(body);
    if (text.contains(QStringLiteral("captcha"), Qt::CaseInsensitive))
        return true;
    // API-walled hosts answer JSON, not HTML (soybooru: HTTP 449
    // {"error":"pow_required"} / {"error":"captcha_required"}).
    if (text.contains(QStringLiteral("pow_required"), Qt::CaseInsensitive) ||
        text.contains(QStringLiteral("captcha_required"),
                      Qt::CaseInsensitive))
        return true;
    return text.contains(QStringLiteral("verification"), Qt::CaseInsensitive) &&
           (text.contains(QStringLiteral("challenge"), Qt::CaseInsensitive) ||
            text.contains(QStringLiteral("powSeed")) ||
            text.contains(QStringLiteral("cloudflare"), Qt::CaseInsensitive));
}

bool looksLikeMediaBytes(const QByteArray &head) {
    if (head.size() < 4)
        return false;
    const auto at = [&](int i, int v) {
        return i < head.size() &&
               static_cast<unsigned char>(head.at(i)) ==
                   static_cast<unsigned char>(v);
    };
    if (at(0, 0xFF) && at(1, 0xD8) && at(2, 0xFF))
        return true; // JPEG
    if (at(0, 0x89) && at(1, 'P') && at(2, 'N') && at(3, 'G'))
        return true; // PNG
    if (head.startsWith("GIF87a") || head.startsWith("GIF89a"))
        return true;
    if (head.size() >= 12 && head.startsWith("RIFF") &&
        head.mid(8, 4) == "WEBP")
        return true;
    if (at(0, 'B') && at(1, 'M'))
        return true; // BMP
    if (head.size() >= 12 && head.mid(4, 4) == "ftyp")
        return true; // AVIF/HEIF/MP4/MOV
    if (at(0, 0x1A) && at(1, 0x45) && at(2, 0xDF) && at(3, 0xA3))
        return true; // WebM/MKV (EBML)
    if (at(0, 0xFF) && at(1, 0x0A))
        return true; // raw JXL codestream
    if (head.size() >= 12 && head.mid(4, 8) == "JXL     ")
        return true; // JXL container
    return false;
}

// One-line, actionable media error for an HTML body where bytes
// were expected: names a recognized wall and points at cookie
// capture, else falls back to the bare content type.
QString wallAwareMediaError(const QString &host, const QByteArray &body,
                            const QString &contentType) {
    if (RemotePosts::looksLikeCloudflareWall(body) || looksLikeChallengePage(body)) {
        const QString where =
            host.isEmpty()
                ? QCoreApplication::translate("RemotePosts", "The server")
                : QCoreApplication::translate("RemotePosts",
                                             "Challenge page from %1")
                      .arg(host);
        return QCoreApplication::translate(
                   "RemotePosts",
"%1 instead of media (captcha/wall still active \u2014 "
                  "solve it once in the Booru manager \u2192 Capture in "
                  "browser\u2026, Apply, then retry).")
            .arg(where);
    }
    return QCoreApplication::translate("RemotePosts",
                                     "Unexpected content type: %1")
        .arg(contentType);
}
} // namespace RemotePosts

QString RemoteSource::powWallError(bool captcha,
                                     const QString &webNote) const {
    const QString host = QUrl(m_baseUrl).host();
    QString err;
    if (m_site.cookies.isEmpty()) {
        err = (captcha
                   ? tr("HTTP 449 \u2014 captcha required by %1. This entry "
                        "has no cookies saved: solve the captcha in "
                        "the capture browser (search on the site so "
                        "it appears), Apply, then retry.")
                   : tr("HTTP 449 \u2014 proof-of-work required by %1. "
                        "This entry has no cookies saved: open the "
                        "capture browser and search on the site so "
                        "the challenge runs (it self-solves), Apply, "
                        "then retry."));
    } else {
        err = (captcha
                   ? tr("HTTP 449 \u2014 captcha required by %1. The saved "
                        "clearance was rejected or expired: "
                        "re-capture (search on the site so the "
                        "challenge appears), Apply, then retry.")
                   : tr("HTTP 449 \u2014 proof-of-work required by %1. "
                        "The saved clearance was rejected or "
                        "expired: re-capture (search on the site so "
                        "the challenge runs), Apply, then retry."));
    }
    err = err.arg(host);
    if (!webNote.isEmpty())
        err += u' ' + webNote;
    if (const RemotePosts::SiteQuirk *q =
            RemotePosts::quirkForHost(host.toLower())) {
        if (q->powWall)
            err += tr(" This site rotates its proof-of-work "
                      "clearance even when logged in: reload it "
                      "via Booru manager \u2192 Refresh (no puzzle), "
                      "Apply, then retry.");
    }
    if (!captcha && !m_lastPowError.isEmpty())
        err += tr(" (automatic proof-of-work failed: %1)")
                   .arg(m_lastPowError);
    if (!captcha &&
        RemotePosts::proxyLooksLikeTor(RemotePosts::proxyForUrl(m_baseUrl)))
        err += tr(" (Tor rotates exit IPs and this clearance is IP-bound: "
                  "set this booru's proxy to direct).");
    return err;
}

QUrl RemoteSource::buildUrl(const QStringList &tags, int page, int limit) const {
    return engineOps(m_site.engine)->searchUrl(opsContext(), tags, page, limit);
}

void RemoteSource::search(const QStringList &tags, int page, int limit) {
    cancel();
    ++m_searchGen; // in-flight native solves from the old search die here
    m_webRetried = false;
    m_powRetried = false;
    m_lastPowError.clear();
    m_lastFreshCookies.clear();
    m_lastTags = tags;
    m_lastUrl.clear();
    m_lastWallKind.clear();
    m_lastBody.clear();
    m_lastError.clear();
    m_lastPosts.clear();
    m_lastHttp = 0;
    m_lastWall = false;
    if (!m_baseUrl.isEmpty() && !RemotePosts::isHttpUrl(m_baseUrl)) {
        SearchResult result;
        result.site = m_site.key;
        result.error = QStringLiteral("Unsupported URL scheme (want http(s))");
        // Deliver async so callers see uniform behavior.
        QTimer::singleShot(0, this, [this, result] { emit searchFinished(result); });
        return;
    }
    if ((m_site.engine == BooruEngine::Szurubooru ||
         m_site.engine == BooruEngine::Shimmie) &&
        m_baseUrl.isEmpty()) {
        SearchResult result;
        result.site = m_site.key;
        result.error = QStringLiteral("Set the %1 instance URL in Preferences → Booru accounts")
                           .arg(m_site.label);
        // Deliver async so callers see uniform behavior.
        QTimer::singleShot(0, this, [this, result] { emit searchFinished(result); });
        return;
    }
    m_pendingPage = qMax(1, page);
    m_pendingLimit = limit;
    QNetworkRequest req = timedRequest(buildUrl(tags, page, limit));
    m_lastUrl = req.url();
    attachSiteCookies(req, m_site.baseUrl, m_site.cookies);
    m_lastCookieNames.clear();
    for (const QByteArray &tok : req.rawHeader("Cookie").split(';')) {
        const QString name =
            QString::fromUtf8(tok.trimmed()).section(u'=', 0, 0).trimmed();
        if (!name.isEmpty())
            m_lastCookieNames << name;
    }
    engineOps(m_site.engine)->applyAuth(req, m_creds);
    m_reply = m_nam->get(req);
    connect(m_reply, &QNetworkReply::finished, this, [this] {
        SearchResult result;
        result.site = m_site.key;
        QNetworkReply *reply = m_reply;
        m_reply = nullptr;
        if (!reply) {
            result.error = QStringLiteral("cancelled");
        } else if (reply->error() != QNetworkReply::NoError) {
            result.httpStatus =
                reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const int http =
                reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            // Include the API's own message when it sent one (szuru/gelbooru
            // JSON error bodies name the real cause, e.g. bad filters).
            // The full body is kept for diagnostics (Test response viewer).
            const QByteArray body = reply->readAll();
            m_lastBody = body.left(32768);
            const QString detail =
                QString::fromUtf8(body).simplified().left(200);
            const QString what =
                http > 0 ? QStringLiteral("HTTP %1 (%2)").arg(http).arg(
                               reply->errorString())
                          : QStringLiteral("Network error: %1")
                                .arg(reply->errorString());
            if (http == 403 &&
                (RemotePosts::looksLikeCloudflareWall(body) ||
                 RemotePosts::looksLikeChallengePage(body))) {
                // Challenge wall (Cloudflare "Just a moment", captchas,
                // rule34.xxx's main-host wall): the HTML body names nothing
                // actionable, so say what to do - including whether any
                // cookies were even sent, which decides capture vs Apply.
                // PoW-walled hosts get one silent browser-backed retry
                // first (its TLS stack passes where QNAM's cannot).
                QString webNote;
                if (tryWebRetry(webNote)) {
                    reply->deleteLater();
                    return;
                }
                if (m_site.cookies.isEmpty()) {
                    result.error =
                        tr("HTTP 403 - challenge from %1. This entry has no "
                           "cookies saved: capture them (Booru manager → "
                           "Capture in browser…), Apply, then retry.")
                            .arg(QUrl(m_baseUrl).host());
                } else {
                    result.error =
                        tr("HTTP 403 - challenge from %1. The saved clearance "
                           "was rejected or expired: re-capture (Booru "
                           "manager → Capture in browser…), Apply, then "
                           "retry.")
                            .arg(QUrl(m_baseUrl).host());
                }
                m_lastWall = true;
                m_lastWallKind = QStringLiteral("challenge");
                if (!webNote.isEmpty())
                    result.error += u' ' + webNote;
                if (m_baseUrl.contains(QStringLiteral("rule34.xxx")))
                    result.error +=
                        tr(" User ID must be the number on the API credentials "
                           "page, not your login; anonymous access is not allowed.");
            } else if (http == 449 &&
                       RemotePosts::looksLikeChallengePage(body)) {
                // API-gated hosts (soybooru): HTTP 449 + pow_required /
                // captcha_required JSON. Bare JSON never runs site JS, so
                // the hidden browser retry only helps when the body is a
                // real (Cloudflare) page; otherwise say how to solve.
                const bool captcha = QString::fromUtf8(body).contains(
                    QStringLiteral("captcha_required"), Qt::CaseInsensitive);
                m_lastWallKind = captcha ? QStringLiteral("captcha")
                                         : QStringLiteral("pow");
                if (!captcha && !m_powRetried) {
                    // IP-bound token: solve on this exact stack (same
                    // proxy and egress, same cookie jar), then retry once.
                    m_powRetried = true;
                    const int gen = m_searchGen;
                    const QStringList retryTags = m_lastTags;
                    const int retryPage = m_pendingPage;
                    const int retryLimit = m_pendingLimit;
                    PowSolver::solve(
                        m_nam, m_baseUrl, this,
                        [this, gen, retryTags, retryPage,
                         retryLimit](bool ok, const QStringList &fresh,
                                     const QString &err) {
                            if (gen != m_searchGen)
                                return; // superseded by a newer search
                            if (ok) {
                                m_lastFreshCookies = fresh;
                                search(retryTags, retryPage, retryLimit);
                                m_powRetried = true;
                            } else {
                                m_lastPowError = err;
                                SearchResult powResult;
                                powResult.site = m_site.key;
                                powResult.httpStatus = 449;
                                powResult.error = powWallError(false, {});
                                m_lastWall = true;
                                m_lastError = powResult.error;
                                m_lastHttp = 449;
                                emit searchFinished(powResult);
                            }
                        });
                    reply->deleteLater();
                    return;
                }
                QString webNote;
                const bool htmlish =
                    body.toLower().contains("<html") ||
                    RemotePosts::looksLikeCloudflareWall(body);
                if (htmlish && tryWebRetry(webNote)) {
                    reply->deleteLater();
                    return;
                }
                result.error = powWallError(captcha, webNote);
                m_lastWall = true;
            } else {
                result.error = detail.isEmpty()
                                   ? what
                                   : what + QStringLiteral(" \u2014 ") + detail;
            }
        } else {
            const QByteArray body = reply->readAll();
            m_lastBody = body.left(32768);
            result = parse(body);
            result.site = m_site.key;
            result.httpStatus = reply->attribute(
                QNetworkRequest::HttpStatusCodeAttribute).toInt();
            // HTTP-200 walls (JS proof-of-work pages like ATF's): the parse
            // fails, but the body names the real cause - say so instead of
            // "unparseable response".
            if (!result.ok() && RemotePosts::looksLikeChallengePage(body)) {
                const QString host = QUrl(m_baseUrl).host();
                QString webNote;
                if (tryWebRetry(webNote)) {
                    reply->deleteLater();
                    return;
                }
                result.error =
                    tr("HTTP 200 - verification challenge from %1 instead of "
                       "data. Solve it in the capture browser (Booru manager "
                       "→ Capture in browser…), then retry.")
                        .arg(host);
                m_lastWall = true;
                m_lastWallKind = QStringLiteral("challenge");
                if (const RemotePosts::SiteQuirk *q =
                        RemotePosts::quirkForHost(host.toLower())) {
                    if (q->powWall)
                        result.error +=
                            tr(" This site rotates its proof-of-work "
                               "clearance even when logged in: reload it "
                               "via Booru manager → Refresh (no puzzle), "
                               "Apply, then retry.");
                }
                if (!webNote.isEmpty())
                    result.error += u' ' + webNote;
            }
        }
        reply->deleteLater();
        m_lastPosts = result.posts;
        m_lastError = result.error;
        m_lastHttp = result.httpStatus;
        emit searchFinished(result);
    });
}

void RemoteSource::cancel() {
    ++m_searchGen; // a pending native solve reports into the void, not here
    if (m_reply) {
        disconnect(m_reply, nullptr, this, nullptr);
        m_reply->abort();
        m_reply->deleteLater();
        m_reply = nullptr;
    }
}

QJsonObject PostRefresher::parseSingle(BooruEngine engine,
                                       const QByteArray &body, bool &ok) {
    return engineOps(engine)->parseSingle(body, ok);
}

RefreshTarget resolveRefreshTarget(const Post &post) {
    RefreshTarget t;
    if (post.isRemote) {
        // Stamped rows carry the exact entry key: a custom instance of a
        // family (ATF vs gelbooru.com) must refresh against its own host,
        // not whatever the display label resolves to. Favourites and
        // older rows without a stamp fall back to the label.
        QString key = post.siteKey;
        if (key.isEmpty() || !RemoteSource::siteFor(key))
            key = RemotePosts::keyForLabel(post.siteName);
        const BooruSite *site = key.isEmpty() ? nullptr : RemoteSource::siteFor(key);
        if (!site)
            return t;
        t.siteKey = key;
        t.baseUrl = site->baseUrl; // empty = self-hosted, not configured
        t.id = post.id;
        t.creds.user = site->user;
        t.creds.secret = site->secret;
        t.cookies = site->cookies;
        t.proxy = site->proxy;
        return t;
    }
    // Local LOSN sidecars carry the post URL + server id.
    if (post.postUrl.isEmpty() || post.id <= 0)
        return t;
    const QString host = QUrl(post.postUrl).host().toLower();
    QString key;
    if (host.contains(QStringLiteral("danbooru")))
        key = QStringLiteral("danbooru");
    else if (host.contains(QStringLiteral("gelbooru")) && !host.contains(QStringLiteral("safe")))
        key = QStringLiteral("gelbooru");
    else if (host.contains(QStringLiteral("safebooru")))
        key = QStringLiteral("safebooru");
    else if (host.contains(QStringLiteral("yande.re")))
        key = QStringLiteral("yandere");
    else if (host.contains(QStringLiteral("konachan")))
        key = QStringLiteral("konachan");
    else if (host.contains(QStringLiteral("aibooru")))
        key = QStringLiteral("aibooru");
    else
        return t;
    const BooruSite *site = RemoteSource::siteFor(key);
    if (!site)
        return t;
    t.siteKey = key;
    t.baseUrl = site->baseUrl;
    t.id = post.id;
    t.creds.user = site->user;
    t.creds.secret = site->secret;
    t.cookies = site->cookies;
    t.proxy = site->proxy;
    return t;
}

PostRefresher::PostRefresher(QObject *parent)
    : QObject(parent), m_nam(new QNetworkAccessManager(this)) {}

QNetworkAccessManager *PostRefresher::namForProxy(const QString &proxySpec) {
    if (proxySpec.trimmed().isEmpty())
        return m_nam;
    auto it = m_proxyNams.find(proxySpec);
    if (it != m_proxyNams.end())
        return it.value();
    const QNetworkProxy proxy = RemotePosts::proxyFromSpec(proxySpec);
    if (proxy.type() == QNetworkProxy::NoProxy)
        return m_nam;
    QNetworkAccessManager *nam = new QNetworkAccessManager(this);
    nam->setProxy(proxy);
    m_proxyNams.insert(proxySpec, nam);
    return nam;
}

void PostRefresher::cancel() {
    if (m_reply) {
        disconnect(m_reply, nullptr, this, nullptr);
        m_reply->abort();
        m_reply->deleteLater();
        m_reply = nullptr;
    }
}

void PostRefresher::refresh(const RefreshTarget &target, qint64 reqId) {
    cancel();
    m_target = target;
    m_reqId = reqId;
    if (!target.valid()) {
        emit failed(m_reqId, QStringLiteral("Nothing refreshable (unknown origin)"));
        return;
    }
    // Shape follows the engine, not the key: custom instances included.
    const BooruEngine engine = RemotePosts::engineForSiteKey(target.siteKey);
    // Fingerprint-walled hosts: plain requests get wall pages while the
    // capture profile passes, so go straight through the browser stack
    // (same fallback search() uses after a wall, without the dead round
    // trip). Proxy env is inherited by the profile, not the entry.
    const RemotePosts::SiteQuirk *wq = RemotePosts::quirkForHost(
        QUrl(target.baseUrl).host().toLower());
    if (wq && wq->powWall) {
        if (const RemoteSource::WebFetchFn web = RemoteSource::webFetcher()) {
            const BooruEngineOps *wops = engineOps(engine);
            const BooruEngineOps::Context wctx{engine, target.baseUrl,
                                               target.creds};
            const QUrl wurl = wops->refreshUrl(wctx, target.id);
            const RemoteSource::WebFetchResult fetched =
                wurl.isEmpty() ? RemoteSource::WebFetchResult{}
                               : web(wurl, 30000);
            bool ok = false;
            const QJsonObject raw =
                fetched.body.isEmpty() ? QJsonObject()
                                       : wops->parseSingle(fetched.body, ok);
            if (fetched.error.isEmpty() && ok) {
                emit refreshed(m_reqId, raw);
                return;
            }
            emit failed(m_reqId,
                        fetched.error.isEmpty()
                            ? QStringLiteral(
                                  "No such post (or unexpected response)")
                            : fetched.error);
            return;
        }
    }
    const BooruEngineOps *ops = engineOps(engine);
    const BooruEngineOps::Context ctx{engine, target.baseUrl, target.creds};
    QNetworkRequest req = timedRequest(ops->refreshUrl(ctx, target.id));
    attachSiteCookies(req, target.baseUrl, target.cookies);
    ops->applyAuth(req, target.creds);
    m_reply = namForProxy(target.proxy)->get(req);
    connect(m_reply, &QNetworkReply::finished, this, [this] {
        QNetworkReply *reply = m_reply;
        m_reply = nullptr;
        if (!reply)
            return; // cancelled by a newer refresh: stay silent, it owns
                    // the UI now (no stale "cancelled" flash)
        if (reply->error() != QNetworkReply::NoError) {
            const int http =
                reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const qint64 failedId = m_reqId;
            const QString detail =
                QString::fromUtf8(reply->readAll()).simplified().left(200);
            const QString what =
                http > 0 ? QStringLiteral("HTTP %1 (%2)").arg(http).arg(reply->errorString())
                         : QStringLiteral("Network error: %1").arg(reply->errorString());
            emit failed(failedId, detail.isEmpty()
                                            ? what
                                            : what + QStringLiteral(" - ") + detail);
        } else {
            bool ok = false;
            const QByteArray refreshBody = reply->readAll();
            if (refreshBody.size() > 32 * 1024 * 1024) {
                emit failed(m_reqId, QStringLiteral("Response too large"));
            } else {
                const QJsonObject raw = parseSingle(
                    RemotePosts::engineForSiteKey(m_target.siteKey),
                    refreshBody, ok);
                if (ok)
                    emit refreshed(m_reqId, raw);
                else
                    emit failed(m_reqId, QStringLiteral(
                                          "No such post (or unexpected response)"));
            }
        }
        reply->deleteLater();
    });
}

bool RemoteSource::testCredentials(const BooruSite &site, const SiteCreds &creds,
                                   QString &message) {
    const CredentialProbe probe = probeCredentials(site, creds);
    message = probe.message;
    return probe.ok;
}

RemoteSource::CredentialProbe
RemoteSource::probeCredentials(const BooruSite &site, const SiteCreds &creds) {
    CredentialProbe out;
    out.siteLabel = site.label.isEmpty() ? site.key : site.label;
    RemoteSource src(site);
    src.setCreds(creds);
    QEventLoop loop;
    QObject::connect(&src, &RemoteSource::searchFinished, &src,
                     [&](const SearchResult &r) {
                         if (!r.ok()) {
                             out.message = r.error;
                         } else if (r.posts.isEmpty()) {
                             out.ok = true;
                             out.message =
                                 QStringLiteral("OK (empty result, auth accepted)");
                         } else {
                             out.ok = true;
                             out.message =
                                 QStringLiteral("OK - %1 resolves as #%2")
                                     .arg(r.posts.first().siteName)
                                     .arg(r.posts.first().id);
                         }
                         out.http = r.httpStatus;
                         out.url = src.lastUrl();
                         out.body = src.lastBody();
                         out.sentCookieNames = src.lastCookieNames();
                         loop.quit();
                     });
    src.search({QStringLiteral("hatsune_miku")}, 1, 1);
    QTimer::singleShot(20000, &loop, &QEventLoop::quit);
    loop.exec();
    src.cancel();
    if (out.message.isEmpty())
        out.message = QStringLiteral("Timed out");
    return out;
}

SearchResult RemoteSource::parse(const QByteArray &body) const {
    // A rogue endpoint can stream gigabytes: refuse absurd API bodies
    // before parsing (media files take the download path, never this).
    if (body.size() > 32 * 1024 * 1024) {
        SearchResult result;
        result.site = m_site.key;
        result.error = QStringLiteral("Response too large");
        return result;
    }
    return engineOps(m_site.engine)->parseSearch(m_site.key, opsContext(), body);
}

bool RemoteSource::tryWebRetry(QString &note) {
    const RemotePosts::SiteQuirk *q =
        RemotePosts::quirkForHost(QUrl(m_baseUrl).host().toLower());
    if (!q || !q->powWall || m_webRetried || m_lastUrl.isEmpty() ||
        !webFetcher())
        return false;
    m_webRetried = true;
    const WebFetchResult web = webFetcher()(m_lastUrl, 30000);
    if (web.error.isEmpty() && !web.body.isEmpty()) {
        SearchResult result = parse(web.body);
        result.site = m_site.key;
        result.httpStatus = web.http;
        if (result.ok()) {
            m_lastBody = web.body.left(32768);
            m_lastPosts = result.posts;
            m_lastError.clear();
            m_lastHttp = result.httpStatus;
            emit searchFinished(result);
            return true;
        }
    }
    note = web.error.isEmpty()
        ? tr("(browser fallback also hit the wall)")
        : tr("(browser fallback: %1)").arg(web.error);
    return false;
}

namespace RemotePosts {

QString keyForLabel(const QString &siteLabel) {
    const QString custom = labelKeyMap().value(siteLabel);
    if (!custom.isEmpty())
        return custom;
    if (siteLabel == QStringLiteral("Danbooru"))
        return QStringLiteral("danbooru");
    if (siteLabel == QStringLiteral("Gelbooru"))
        return QStringLiteral("gelbooru");
    if (siteLabel == QStringLiteral("Safebooru"))
        return QStringLiteral("safebooru");
    if (siteLabel == QStringLiteral("Yandere"))
        return QStringLiteral("yandere");
    if (siteLabel == QStringLiteral("Konachan"))
        return QStringLiteral("konachan");
    if (siteLabel == QStringLiteral("AIBooru"))
        return QStringLiteral("aibooru");
    if (siteLabel == QStringLiteral("Szurubooru"))
        return QStringLiteral("szurubooru");
    if (siteLabel == QStringLiteral("Shimmie"))
        return QStringLiteral("shimmie");
    return {};
}

QMap<QString, QString> hostKeys() {
    // Configured entries win on collision (same rule as labelKeys()).
    QMap<QString, QString> out;
    auto add = [&](const BooruSite &b) {
        if (b.key.isEmpty() || b.baseUrl.isEmpty())
            return;
        const QString host = QUrl(b.baseUrl).host().toLower();
        if (!host.isEmpty())
            out.insert(host, b.key);
    };
    for (const BooruSite &b : RemoteSource::presetBoorus())
        add(b);
    for (auto it = labelKeyMap().begin(); it != labelKeyMap().end(); ++it) {
        const BooruSite *site = RemoteSource::siteFor(it.value());
        if (site)
            add(*site);
    }
    return out;
}

const BooruSite *entryForPost(const Post &post) {
    if (post.isRemote) {
        if (!post.siteKey.isEmpty()) {
            if (const BooruSite *e = RemoteSource::siteFor(post.siteKey))
                return e;
        }
        const QString key = keyForLabel(post.siteName);
        return key.isEmpty() ? nullptr : RemoteSource::siteFor(key);
    }
    // Strays and plain library folders resolve to nothing.
    if (post.category.isEmpty() || post.category == QStringLiteral("_Stray"))
        return nullptr;
    // Legacy label folders first, then host-named download folders.
    const QString labelKey = keyForLabel(post.category);
    if (!labelKey.isEmpty()) {
        if (const BooruSite *e = RemoteSource::siteFor(labelKey))
            return e;
    }
    return RemoteSource::siteFor(
        hostKeys().value(post.category.toLower()));
}

QString postPageUrlFor(const BooruSite &site, qint64 id) {
    if (site.baseUrl.isEmpty() || id <= 0)
        return {};
    const QString base = site.baseUrl;
    using enum BooruEngine;
    switch (site.engine) {
    case Danbooru:
        return base + QStringLiteral("/posts/") + QString::number(id);
    case Moebooru:
        return base + QStringLiteral("/post/show/") + QString::number(id);
    case Szurubooru:
        return base + QStringLiteral("/post/") + QString::number(id);
    case Shimmie:
        return base + QStringLiteral("/post/view/") + QString::number(id);
    case Hybooru:
        return base + QStringLiteral("/posts/") + QString::number(id);
    case Nuuru:
        return base + QStringLiteral("/post/view/") + QString::number(id);
    case Derpibooru:
        return base + QStringLiteral("/images/") + QString::number(id);
    case Twibooru:
        return base + QStringLiteral("/posts/") + QString::number(id);
    case Gelbooru:
    case Safebooru:
        return base + QStringLiteral("/index.php?page=post&s=view&id=") +
               QString::number(id);
    }
    return {};
}

bool needsWebDownload(const Post &post) {
    const QString key = !post.siteKey.isEmpty() ? post.siteKey
                                                : keyForLabel(post.siteName);
    if (const BooruSite *site =
            key.isEmpty() ? nullptr : RemoteSource::siteFor(key)) {
        if (const SiteQuirk *q = quirkForHost(
                QUrl(site->baseUrl).host().toLower())) {
            if (q->powWall)
                return true;
        }
    }
    if (!post.fileUrl.isEmpty()) {
        if (const SiteQuirk *q =
                quirkForHost(QUrl(post.fileUrl).host().toLower()))
            return q->powWall;
    }
    return false;
}

QString dragUrlFor(const Post &post, DragLinkMode globalMode) {
    if (!post.isRemote)
        return {};
    DragLinkMode mode = globalMode;
    if (const BooruSite *site = entryForPost(post);
        site && site->dragLinkMode != DragLinkMode::Inherit)
        mode = site->dragLinkMode;
    const bool postOk = !post.postUrl.isEmpty() && isHttpUrl(post.postUrl);
    const bool fileOk = !post.fileUrl.isEmpty() && isHttpUrl(post.fileUrl);
    if (mode == DragLinkMode::DirectMedia) {
        if (fileOk)
            return post.fileUrl;
        if (postOk)
            return post.postUrl;
    } else {
        if (postOk)
            return post.postUrl;
        if (fileOk)
            return post.fileUrl;
    }
    return {};
}

QString fsTagForPost(const Post &post) {
    // Strays are homeless by design: no site, no host.
    if (post.siteName == QStringLiteral("_Stray"))
        return post.siteName;
    const QString key = !post.siteKey.isEmpty() ? post.siteKey
                                                : keyForLabel(post.siteName);
    const BooruSite *site =
        key.isEmpty() ? nullptr : RemoteSource::siteFor(key);
    if (site && !site->baseUrl.isEmpty()) {
        const QUrl u(site->baseUrl);
        const QString host = u.host().toLower();
        if (!host.isEmpty()) {
            // Self-hosted instances often live behind explicit ports
            // (localhost:8080 vs :9090): keep those distinct, drop the
            // scheme defaults.
            const int port = u.port();
            const QString scheme = u.scheme().toLower();
            const bool nonDefault =
                port > 0 && !((scheme == QStringLiteral("http") && port == 80) ||
                              (scheme == QStringLiteral("https") && port == 443));
            if (nonDefault)
                return host + u'_' + QString::number(port);
            return host;
        }
    }
    return post.siteName; // unconfigured/unknown: legacy label layout
}

QString mediaReferer(const QString &siteKey) {
    const BooruSite *site = RemoteSource::siteFor(siteKey);
    if (!site || site->engine != BooruEngine::Gelbooru || site->baseUrl.isEmpty())
        return {};
    QString base = site->baseUrl;
    if (!base.endsWith(u'/'))
        base += u'/';
    return base;
}

QString mediaRefererFor(const Post &post) {
    if (!post.postUrl.isEmpty()) {
        const QUrl u(post.postUrl);
        if (!u.host().isEmpty()) {
            const QString scheme =
                u.scheme().isEmpty() ? QStringLiteral("https") : u.scheme();
            return scheme + QStringLiteral("://") + u.host().toLower() + u'/';
        }
    }
    return mediaReferer(keyForLabel(post.siteName));
}

BooruEngine engineForSiteKey(const QString &key) {
    const BooruSite *site = RemoteSource::siteFor(key);
    return site ? site->engine : BooruEngine::Danbooru;
}

bool skipAutoClassify(const Post &post) {
    if (!post.isRemote)
        return false;
    QString key = !post.siteKey.isEmpty() ? post.siteKey
                                          : keyForLabel(post.siteName);
    const BooruSite *site =
        key.isEmpty() ? nullptr : RemoteSource::siteFor(key);
    return site && site->skipAutoClassify;
}

QString foldedBucketFor(const Post &post, const QString &bucket) {
    if (bucket.isEmpty() || bucket == QStringLiteral("general"))
        return bucket;
    const QString key = !post.siteKey.isEmpty() ? post.siteKey
                                                : keyForLabel(post.siteName);
    const BooruSite *site =
        key.isEmpty() ? nullptr : RemoteSource::siteFor(key);
    if (!site)
        return bucket;
    if (site->disabledBuckets.contains(bucket) ||
        RemotePosts::bucketDisabled(site->engine, bucket))
        return QStringLiteral("general");
    return bucket;
}

namespace {
QString &probedCaptureUa() {
    static QString ua;
    return ua;
}
} // namespace

QString captureUserAgent() {
    // Shared by the capture browser and every request carrying captured
    // cookies (see applyCookies): both ends send byte-identical strings,
    // since Cloudflare binds clearance to the solving UA. Engine default
    // once probed (see setCaptureUserAgent), pinned Chrome before that.
    const QString &probed = probedCaptureUa();
    if (!probed.isEmpty())
        return probed;
    return QStringLiteral(
        "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
        "(KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36");
}

void setCaptureUserAgent(const QString &ua) {
    // Stored verbatim except the QtWebEngine version token: a stock
    // Chrome/140 string + matching TLS fingerprint looks like any other
    // desktop browser, while a rare engine marker invites heuristics.
    QString clean = ua.trimmed();
    clean.remove(QRegularExpression(QStringLiteral("QtWebEngine/[^ ]* ?")));
    if (!clean.isEmpty())
        probedCaptureUa() = clean;
}

QString redactSecrets(const QString &url) {
    // api_key=SECRET&user_id=7 -> api_key=<redacted>&user_id=7.
    // Only values of credential parameters are masked; names, hosts,
    // paths and all other parameters stay readable.
    static const QRegularExpression re(
        QStringLiteral("((?:api_key|pass_?hash|pass_?word|token|secret)=)[^&\\s]*"),
        QRegularExpression::CaseInsensitiveOption);
    QString out = url;
    out.replace(re, QStringLiteral("\\1<redacted>"));
    return out;
}

QString probeReportText(const QString &url, int http, const QByteArray &body) {
    QString text = QStringLiteral("URL: %1\nHTTP: %2\n\n")
                       .arg(url, http > 0 ? QString::number(http)
                                          : QStringLiteral("no response"));
    if (body.isEmpty()) {
        text += QStringLiteral("(empty body)");
    } else {
        static constexpr int kShowBody = 8192;
        text += QString::fromUtf8(body.left(kShowBody));
        if (body.size() > kShowBody)
            text += QStringLiteral("\n…(+%1 bytes truncated)")
                        .arg(body.size() - kShowBody);
    }
    return text;
}

void showProbeDialog(QWidget *parent, const QString &title, const QString &text) {
    QDialog dlg(parent);
    dlg.setWindowTitle(title);
    dlg.resize(640, 480);
    auto *lay = new QVBoxLayout(&dlg);
    auto *view = new QPlainTextEdit(&dlg);
    view->setReadOnly(true);
    view->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    view->setTabChangesFocus(true);
    view->setPlainText(text);
    lay->addWidget(view, 1);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dlg);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    lay->addWidget(buttons);
    dlg.exec();
}

} // namespace RemotePosts
