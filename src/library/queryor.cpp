#include "queryor.h"

namespace QueryOr {

ParsedQuery parse(const QString &query) {
    ParsedQuery out;
    // Cursor scan over a bounded window: the old code re-copied the
    // shrinking remainder with mid() per group (quadratic on hostile
    // ~(a)~(b)... chains from the box or restored tabs). Past the caps the
    // remainder stays one literal AND blob (still searchable, just not
    // group-split).
    static constexpr qsizetype kMaxChars = 65536;
    static constexpr int kMaxGroups = 512;
    const QStringView view(query);
    const qsizetype n = qMin(view.size(), kMaxChars);
    qsizetype pos = 0;
    const bool truncated = view.size() > kMaxChars;
    auto pushAnd = [&](QStringView span) {
        for (const QString &tok : span.toString().split(
                 QChar::Space, Qt::SkipEmptyParts))
            out.andTokens << tok;
    };
    while (pos < n) {
        const qsizetype open = view.indexOf(QStringLiteral("~("), pos);
        if (open < 0 || open + 2 >= n || out.groups.size() >= kMaxGroups)
            break;
        pushAnd(view.mid(pos, open - pos));
        const qsizetype close = view.indexOf(u')', open + 2);
        if (close < 0 || close >= n) {
            // Unterminated: keep the remainder literal, stop parsing groups
            // (and skip the lone-~ split, like before).
            pushAnd(view.mid(open, n - open));
            if (truncated)
                pushAnd(view.mid(n));
            return out;
        }
        QStringList alts;
        for (const QString &tok : view.mid(open + 2, close - open - 2)
                     .toString()
                     .split(QChar::Space, Qt::SkipEmptyParts))
            alts << tok;
        out.groups << alts;
        pos = close + 1;
    }
    if (pos < n)
        pushAnd(view.mid(pos, n - pos));
    if (truncated)
        pushAnd(view.mid(n));
    // A lone `~tag` is a one-option group (Danbooru users expect `~` OR).
    QStringList kept;
    kept.reserve(out.andTokens.size());
    for (const QString &tok : out.andTokens) {
        if (tok.startsWith(u'~') && tok.size() > 1 && !tok.startsWith(QStringLiteral("~("))) {
            out.groups << QStringList{tok.mid(1)};
        } else {
            kept << tok;
        }
    }
    out.andTokens = kept;
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
    // Bounded: per-keystroke path, so a pasted megabyte of spaces must not
    // become a million-element list (SkipEmptyParts + tail window). A
    // trailing space still means "no token in progress".
    if (queryText.endsWith(QChar::Space))
        return {};
    const QString tail = queryText.right(512);
    const QStringList parts = tail.split(QChar::Space, Qt::SkipEmptyParts);
    if (parts.isEmpty())
        return {};
    QString last = parts.last();
    if (last.startsWith(u'-') && last.size() > 1)
        last = last.mid(1);
    // Pool reference (`12#tag`, Danbooru pool syntax): complete the tag.
    if (last.size() > 2 && last.at(0).isDigit()) {
        const int hash = last.indexOf(u'#');
        if (hash > 0)
            last = last.mid(hash + 1);
    }
    if (last.startsWith(QStringLiteral("~(")))
        last = last.mid(2);
    else if (last.startsWith(u'~') || last.startsWith(u'('))
        last = last.mid(1);
    while (last.endsWith(u')') && last.size() > 1)
        last.chop(1);
    return last;
}

QStringList pushRecentTag(QStringList recent, const QString &tag,
                              int cap) {
    const QString t = tag.trimmed();
    if (t.isEmpty() || cap <= 0)
        return recent;
    recent.removeAll(t);
    recent.prepend(t);
    while (recent.size() > cap)
        recent.removeLast();
    return recent;
}

} // namespace QueryOr
