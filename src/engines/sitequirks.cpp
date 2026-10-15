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

} // namespace RemotePosts
