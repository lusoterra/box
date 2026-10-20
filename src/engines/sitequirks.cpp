// Per-host quirks, moved verbatim out of remotesource.cpp.
#include "sitequirks.h"

namespace RemotePosts {

QList<SiteQuirk> siteQuirks() {
    return {
        {QStringLiteral("gelbooru.com"), QString(),
         {QStringLiteral("fringeBenefits=yup")}, false},
        {QStringLiteral("rule34.xxx"),
         QStringLiteral("https://api.rule34.xxx"), {}, false},
        {QStringLiteral("booru.allthefallen.moe"), QString(), {},
         true},
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

} // namespace RemotePosts
