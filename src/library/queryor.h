#pragma once

// Tag OR-groups for search: `~(konata_izumi izumi_konata)` matches posts
// carrying either tag (rule34.xxx reverses some character names, so one
// spelling never finds both). Groups compose with plain AND terms and
// `-` exclusions; a lone `~tag` is a one-option group (Danbooru parity).
//
// Engines without a native OR get one search per alternative and the
// results union client-side, so semantics are identical everywhere.
// Pure string logic (unit-tested); the merge lives in MainWindow.
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>

namespace QueryOr {

// Hard cap on cartesian combinations per search (3 groups of 2-3 is
// typical; beyond this the user should narrow the groups, not hammer
// every API with dozens of requests).
inline constexpr int kMaxCombos = 12;

struct ParsedQuery {
    QStringList andTokens;     // plain AND terms (may carry `-`)
    QList<QStringList> groups; // OR alternatives per group (may carry `-`)
};

// Split a raw query string into AND terms + OR groups. An unterminated
// `~(` is kept literal; an empty group parses to an empty alternative
// list (the search then matches nothing).
ParsedQuery parse(const QString &query);

// Cartesian AND-queries: andTokens plus one alternative per group.
// Empty when a group is empty; sets over + returns empty when the
// combination count exceeds cap.
QList<QStringList> expand(const ParsedQuery &parsed, int cap, bool &over);

// Group satisfaction over a post's literal tag set: every group needs
// one positive alternative present and no negative (`-x`) one present.
bool groupsMatch(const QSet<QString> &postTags, const QList<QStringList> &groups);

// Current-tag prefix for autocomplete: the last space-separated token
// with `-` exclusions, `~(...)` group syntax and `N#` pool prefixes
// stripped, so suggestions keep working on the 2nd/3rd tag and inside OR
// groups.
QString suggestBare(const QString &queryText);
// Most-recent-first tag history (token strings): moves an existing entry
// to the front, appends new ones, caps the length. Pure (unit-tested).
QStringList pushRecentTag(QStringList recent, const QString &tag,
                          int cap = 200);

} // namespace QueryOr
