#include "queryor.h"

namespace QueryOr {

ParsedQuery parse(const QString &query) {
    ParsedQuery out;
    QString rest = query;
    while (true) {
        const int open = rest.indexOf(QStringLiteral("~("));
        if (open < 0)
            break;
        // AND part before the group.
        for (const QString &tok : rest.left(open).split(
                 QChar::Space, Qt::SkipEmptyParts))
            out.andTokens << tok;
        const int close = rest.indexOf(u')', open + 2);
        if (close < 0) {
            // Unterminated: keep the remainder literal, stop parsing groups.
            for (const QString &tok : rest.mid(open).split(
                     QChar::Space, Qt::SkipEmptyParts))
                out.andTokens << tok;
            return out;
        }
        QStringList alts;
        for (const QString &tok : rest.mid(open + 2, close - open - 2)
                     .split(QChar::Space, Qt::SkipEmptyParts))
            alts << tok;
        out.groups << alts;
        rest = rest.mid(close + 1);
    }
    for (const QString &tok :
         rest.split(QChar::Space, Qt::SkipEmptyParts))
        out.andTokens << tok;
    // A lone `~tag` is a one-option group (Danbooru users expect `~` OR).
    for (int i = 0; i < out.andTokens.size();) {
        const QString tok = out.andTokens.at(i);
        if (tok.startsWith(u'~') && tok.size() > 1 && !tok.startsWith(QStringLiteral("~("))) {
            out.groups << QStringList{tok.mid(1)};
            out.andTokens.removeAt(i);
        } else {
            ++i;
        }
    }
    return out;
}

QList<QStringList> expand(const ParsedQuery &parsed, int cap, bool &over) {
    over = false;
    QList<QStringList> combos = {parsed.andTokens};
    for (const QStringList &alts : parsed.groups) {
        if (alts.isEmpty())
            return {}; // empty group: nothing can match
        QList<QStringList> next;
        next.reserve(combos.size() * alts.size());
        for (const QStringList &base : combos) {
            for (const QString &alt : alts) {
                if (next.size() >= cap) {
                    over = true;
                    return {};
                }
                next << (base + QStringList{alt});
            }
        }
        combos.swap(next);
    }
    return combos;
}

bool groupsMatch(const QSet<QString> &postTags, const QList<QStringList> &groups) {
    for (const QStringList &alts : groups) {
        bool posEmpty = true;
        bool posHit = false;
        bool negHit = false;
        for (const QString &alt : alts) {
            if (alt.startsWith(u'-') && alt.size() > 1) {
                if (postTags.contains(alt.mid(1)))
                    negHit = true;
            } else if (!alt.isEmpty()) {
                posEmpty = false;
                if (postTags.contains(alt))
                    posHit = true;
            }
        }
        if ((!posEmpty && !posHit) || negHit)
            return false;
    }
    return true;
}

QString suggestBare(const QString &queryText) {
    const QStringList parts = queryText.split(QChar::Space);
    if (parts.isEmpty())
        return {};
    QString last = parts.last();
    if (last.startsWith(u'-') && last.size() > 1)
        last = last.mid(1);
    if (last.startsWith(QStringLiteral("~(")))
        last = last.mid(2);
    else if (last.startsWith(u'~') || last.startsWith(u'('))
        last = last.mid(1);
    while (last.endsWith(u')') && last.size() > 1)
        last.chop(1);
    return last;
}

} // namespace QueryOr
