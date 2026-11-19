// Per-host quirks, moved verbatim out of remotesource.cpp.
#include "sitequirks.h"

#include <algorithm>

namespace RemotePosts {

QList<SiteQuirk> siteQuirks() {
    return {
        {QStringLiteral("gelbooru.com"), QString(),
         {QStringLiteral("fringeBenefits=yup")}, false},
        {QStringLiteral("rule34.xxx"),
         QStringLiteral("https://api.rule34.xxx"), {}, false},
        {QStringLiteral("booru.allthefallen.moe"), QString(), {},
         true},
        {QStringLiteral("soybooru.com"), QString(), {}, true},

        {QStringLiteral("hbooru5cef5tkb7a7m5de5kjxqg6abfh3447sxw36n5pe7utnnei25yd.onion"),
         QString(), {}, true},
    };
}

const SiteQuirk *quirkForHost(const QString &host) {
    const QString h = host.trimmed().toLower();
    if (h.isEmpty())
        return nullptr;
    static const QList<SiteQuirk> quirks = siteQuirks();
    for (const SiteQuirk &q : quirks) {
        if (q.host == h)
            return &q;
    }
    return nullptr;
}

const SiteQuirk *quirkForUrl(const QString &baseUrl) {
    return quirkForHost(QUrl(baseUrl).host());
}

QString defaultBadgeColor(const QString &baseUrl, BooruEngine engine) {
    const QString h = QUrl(baseUrl).host().trimmed().toLower();
    auto subOf = [&](const QString &domain) {
        return !h.isEmpty() && (h == domain || h.endsWith(u'.' + domain));
    };
    if (subOf(QStringLiteral("gelbooru.com")))
        return QStringLiteral("#0870fa");
    if (subOf(QStringLiteral("rule34.xxx")))
        return QStringLiteral("#55aa00");
    if (subOf(QStringLiteral("aibooru.online")))
        return QStringLiteral("#44cc00");
    if (subOf(QStringLiteral("danbooru.donmai.us")) ||
        subOf(QStringLiteral("safebooru.donmai.us")) ||
        subOf(QStringLiteral("safebooru.org")))
        return QStringLiteral("#daac82");
    if (engine == BooruEngine::Shimmie)
        return QStringLiteral("#7f7f7f");
    return QStringLiteral("#3a7ec2");
}

bool isHttpUrl(const QString &url) {
    const QString scheme = QUrl(url).scheme().toLower();
    return scheme == QStringLiteral("http") || scheme == QStringLiteral("https");
}

QString apiBaseUrl(BooruEngine engine, const QString &baseUrl) {
    if (const SiteQuirk *q = quirkForUrl(baseUrl)) {
        if (!q->apiHost.isEmpty() && engine == BooruEngine::Gelbooru)
            return q->apiHost;
    }
    return baseUrl;
}

QStringList normalizeCookies(const QStringList &lines) {
    // Tolerates pasted Cookie headers ("a=1; b=2", possibly multiline)
    // and Set-Cookie lines (attributes after the first ';' dropped).
    QMap<QString, QString> jar; // sorted + deduped, later lines win
    for (const QString &line : lines) {
        for (const QString &tok : line.split(u';')) {
            const QString t = tok.trimmed();
            if (t.isEmpty() || t.startsWith(u'$'))
                continue; // $Path/$Domain relics
            const int eq = t.indexOf(u'=');
            if (eq <= 0)
                continue;
            const QString name = t.left(eq).trimmed();
            const QString value = t.mid(eq + 1).trimmed();
            if (name.isEmpty() || value.isEmpty() || name.contains(u' ') ||
                name.contains(u';') || name.size() > 256 || value.size() > 4096)
                continue;
            // Set-Cookie attributes never survive as names.
            const QString lower = name.toLower();
            if (lower == QStringLiteral("path") || lower == QStringLiteral("domain") ||
                lower == QStringLiteral("expires") || lower == QStringLiteral("max-age") ||
                lower == QStringLiteral("samesite") || lower == QStringLiteral("secure") ||
                lower == QStringLiteral("httponly"))
                continue;
            jar.insert(name, value);
            if (jar.size() >= 50)
                break;
        }
        if (jar.size() >= 50)
            break;
    }
    QStringList out;
    out.reserve(jar.size());
    for (auto it = jar.begin(); it != jar.end(); ++it)
        out << it.key() + u'=' + it.value();
    return out;
}

QString cookieHeaderValue(const QStringList &cookies) {
    return normalizeCookies(cookies).join(QStringLiteral("; "));
}

namespace {
// Cookie-domain semantics for host-scoped entries: the request must be
// the host itself or a subdomain of it (never a parent/sibling).
bool requestUnderHost(const QString &request, const QString &host) {
    return !request.isEmpty() && !host.isEmpty() &&
           (request == host || request.endsWith(u'.' + host));
}
} // namespace

bool isKnownMediaHost(const QString &siteHost, const QString &requestHost) {
    if (siteHost.isEmpty() || requestHost.isEmpty())
        return false;
    if (requestHost == QStringLiteral("cdn.donmai.us") &&
        (siteHost == QStringLiteral("danbooru.donmai.us") ||
         siteHost == QStringLiteral("safebooru.donmai.us") ||
         siteHost == QStringLiteral("donmai.us") ||
         siteHost.endsWith(QStringLiteral(".donmai.us"))))
        return true;
    if ((requestHost == QStringLiteral("derpicdn.net") ||
         requestHost.endsWith(QStringLiteral(".derpicdn.net"))) &&
        (siteHost == QStringLiteral("derpibooru.org") ||
         siteHost.endsWith(QStringLiteral(".derpibooru.org"))))
        return true;
    return false;
}

QString cookieHeaderValueForHost(const QStringList &cookies,
                                 const QString &requestHost,
                                 const QString &siteHost) {
    const QString req = requestHost.trimmed().toLower();
    const QString site = siteHost.trimmed().toLower();
    if (req.isEmpty() || site.isEmpty())
        return {};
    QMap<QString, QString> jar;
    auto insert = [&jar](const QString &entry) {
        const int eq = entry.indexOf(u'=');
        if (eq <= 0)
            return;
        const QString name = entry.left(eq).trimmed();
        const QString value = entry.mid(eq + 1);
        if (!name.isEmpty() && !value.isEmpty())
            jar.insert(name, value);
    };
    const bool direct = requestUnderHost(req, site);
    const bool media = isKnownMediaHost(site, req);
    if (direct || media) {
        for (const QString &entry : cookies) {
            const int eq = entry.indexOf(u'=');
            const int bang = entry.indexOf(u'!');
            if (bang > 0 && (eq < 0 || bang < eq))
                continue; // host-scoped entry
            if (!direct) {
                // Media CDNs only need challenge clearance; never send a
                // site login/session cookie to a sibling host.
                const QString name =
                    (eq > 0 ? entry.left(eq) : entry).trimmed().toLower();
                if (!name.startsWith(QStringLiteral("cf")) &&
                    !name.startsWith(QStringLiteral("__cf")) &&
                    !name.startsWith(QStringLiteral("_cf")))
                    continue;
            }
            insert(entry);
        }
    }
    // Host-scoped entries win on a name collision: the CDN's own
    // cf_clearance must override the site one for CDN requests. Broader
    // domains (donmai.us) insert before narrower hosts (cdn.donmai.us).
    QList<QPair<int, QString>> scoped;
    for (const QString &entry : cookies) {
        const int bang = entry.indexOf(u'!');
        const int eq = entry.indexOf(u'=');
        if (bang <= 0 || (eq > 0 && bang > eq))
            continue;
        const QString scope = entry.left(bang).trimmed().toLower();
        if (!requestUnderHost(req, scope))
            continue;
        const QString nv = entry.mid(bang + 1);
        if (!direct) {
            // Same clearance-only rule for host-scoped entries: a CDN
            // capture must never leak a site session cookie.
            const int scEq = nv.indexOf(u'=');
            const QString scName =
                (scEq > 0 ? nv.left(scEq) : nv).trimmed().toLower();
            if (!scName.startsWith(QStringLiteral("cf")) &&
                !scName.startsWith(QStringLiteral("__cf")) &&
                !scName.startsWith(QStringLiteral("_cf")))
                continue;
        }
        scoped.append({scope.size(), nv});
    }
    std::sort(scoped.begin(), scoped.end(),
              [](const auto &a, const auto &b) { return a.first < b.first; });
    for (const auto &p : scoped)
        insert(p.second);
    if (jar.isEmpty())
        return {};
    QStringList out;
    out.reserve(jar.size());
    for (auto it = jar.begin(); it != jar.end(); ++it)
        out << it.key() + u'=' + it.value();
    return out.join(QStringLiteral("; "));
}

QStringList seedQuirkCookies(const QStringList &current, const QString &host) {
    const SiteQuirk *q = quirkForHost(host);
    QStringList add;
    if (!q)
        return add;
    for (const QString &seed : q->defaultCookies) {
        const QString seedName = seed.section(u'=', 0, 0).trimmed();
        bool has = false;
        for (const QString &c : current + add) {
            if (c.section(u'=', 0, 0).trimmed().compare(seedName,
                                                        Qt::CaseInsensitive) == 0) {
                has = true;
                break;
            }
        }
        if (!has)
            add << seed;
    }
    return add;
}

bool cookieBelongsTo(const QNetworkCookie &cookie, const QString &host) {
    if (host.isEmpty() || cookie.name().isEmpty())
        return false;
    QString domain = cookie.domain().toLower().trimmed();
    if (domain.startsWith(u'.'))
        domain = domain.mid(1);
    if (domain.isEmpty())
        return true; // host-only session cookie: keep, server set it
    return domain == host || host.endsWith(u'.' + domain) ||
           domain.endsWith(u'.' + host);
}

QStringList harvestCookies(const QList<QNetworkCookie> &seen,
                           const QString &host) {
    QStringList raw;
    for (const QNetworkCookie &c : seen) {
        if (!cookieBelongsTo(c, host))
            continue;
        const QString name = QString::fromUtf8(c.name());
        const QString value = QString::fromUtf8(c.value());
        if (!name.isEmpty() && !value.isEmpty())
            raw << name + u'=' + value;
    }
    return normalizeCookies(raw);
}

QStringList harvestScopedCookies(const QList<QNetworkCookie> &seen,
                                 const QString &host,
                                 const QString &siteHost) {
    const QString capture = host.trimmed().toLower();
    const QString site = siteHost.trimmed().toLower();
    if (capture.isEmpty())
        return {};
    const bool captureIsSite =
        !site.isEmpty() &&
        (capture == site || capture.endsWith(u'.' + site));
    auto isChallengeCookie = [](const QString &name) {
        const QString n = name.toLower();
        return n.startsWith(QStringLiteral("cf")) ||
               n.startsWith(QStringLiteral("__cf")) ||
               n.startsWith(QStringLiteral("_cf"));
    };
    QStringList raw;
    for (const QNetworkCookie &c : seen) {
        if (!cookieBelongsTo(c, capture))
            continue;
        const QString name = QString::fromUtf8(c.name());
        const QString value = QString::fromUtf8(c.value());
        if (name.isEmpty() || value.isEmpty())
            continue;
        const bool challenge = isChallengeCookie(name);
        // A sibling-CDN capture replays host-only site cookies from the
        // shared profile; they do not belong to the CDN (only clearance
        // is needed there). Domain cookies are already valid for both.
        if (!challenge && !captureIsSite && !site.isEmpty() &&
            isKnownMediaHost(site, capture) && c.domain().isEmpty())
            continue;
        // Domain cookies keep their scope (Cloudflare often sets
        // .donmai.us); host-only cookies belong to the capture host.
        QString scope = c.domain().toLower().trimmed();
        if (scope.startsWith(u'.'))
            scope = scope.mid(1);
        if (scope.isEmpty())
            scope = capture;
        // A clearance captured on the site also unlocks its media CDN in
        // a browser (same Cloudflare zone). Keep those entry-level when
        // the capture is the site itself or its own domain, so
        // cookieHeaderValueForHost can forward them to the CDN. A
        // capture made on the CDN stays scoped and cannot clobber the
        // site clearance.
        if (challenge && !site.isEmpty() && captureIsSite &&
            (scope == capture || scope == site))
            scope.clear();
        if (scope.isEmpty())
            raw << name + u'=' + value;
        else
            raw << scope + u'!' + name + u'=' + value;
    }
    return normalizeCookies(raw);
}

} // namespace RemotePosts
