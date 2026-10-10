#pragma once

#include <QByteArray>
#include <QCryptographicHash>
#include <QMutex>
#include <QMutexLocker>
#include <QSet>
#include <QString>
#include <QStringList>

// Explicit-tag fallback (in-memory only, source JSON files are never modified).
// Many archives are under-tagged for rating: explicit images stored as
// "general". If a post's rating is NOT already explicit but its tags contain
// any tag from the ACTIVE set, the rating is upgraded to "explicit".
//
// The active set starts as the built-in defaults below and can be replaced
// at runtime (Preferences dialog -> saved to settings.ini). Set it before a
// scan; the scanner bakes a hash of it into the index cache so edits
// automatically invalidate stale ratings.
inline const QSet<QString> &defaultExplicitTags() {
    static const QSet<QString> tags = {
        QStringLiteral("explicit"), QStringLiteral("nsfw"), QStringLiteral("porn"),
        QStringLiteral("hentai"), QStringLiteral("sex"), QStringLiteral("vaginal"),
        QStringLiteral("anal"), QStringLiteral("oral"), QStringLiteral("blowjob"),
        QStringLiteral("deepthroat"), QStringLiteral("fellatio"), QStringLiteral("cunnilingus"),
        QStringLiteral("anilingus"), QStringLiteral("irrumatio"), QStringLiteral("handjob"),
        QStringLiteral("footjob"), QStringLiteral("titjob"), QStringLiteral("paizuri"),
        QStringLiteral("boobjob"), QStringLiteral("masturbation"),
        QStringLiteral("female_masturbation"), QStringLiteral("male_masturbation"),
        QStringLiteral("masturbating"), QStringLiteral("penetration"), QStringLiteral("intercourse"),
        QStringLiteral("vaginal_sex"), QStringLiteral("anal_sex"), QStringLiteral("oral_sex"),
        QStringLiteral("penis"), QStringLiteral("erect_penis"), QStringLiteral("pussy"),
        QStringLiteral("vagina"), QStringLiteral("labia"), QStringLiteral("clitoris"),
        QStringLiteral("testicles"), QStringLiteral("scrotum"), QStringLiteral("nude"),
        QStringLiteral("naked"), QStringLiteral("topless"), QStringLiteral("bottomless"),
        QStringLiteral("cum"), QStringLiteral("semen"), QStringLiteral("ejaculation"),
        QStringLiteral("creampie"), QStringLiteral("facial"), QStringLiteral("bukkake"),
        QStringLiteral("squirting"), QStringLiteral("cum_in_pussy"), QStringLiteral("cum_in_ass"),
        QStringLiteral("cum_on_breasts"), QStringLiteral("cum_on_body"), QStringLiteral("cum_on_face"),
        QStringLiteral("internal_cumshot"), QStringLiteral("dildo"), QStringLiteral("vibrator"),
        QStringLiteral("sex_toy"), QStringLiteral("butt_plug"), QStringLiteral("anal_beads"),
        QStringLiteral("strap_on"), QStringLiteral("fisting"), QStringLiteral("fingering"),
        QStringLiteral("double_penetration"), QStringLiteral("bondage"), QStringLiteral("bdsm"),
        QStringLiteral("shibari"), QStringLiteral("futanari"), QStringLiteral("dickgirl"),
        QStringLiteral("ahegao"), QStringLiteral("orgasm"), QStringLiteral("peeing"),
        QStringLiteral("pissing"), QStringLiteral("watersports"), QStringLiteral("scat"),
        QStringLiteral("tentacle_sex"), QStringLiteral("tentacles"), QStringLiteral("zoophilia"),
        QStringLiteral("bestiality"), QStringLiteral("rape"), QStringLiteral("gangbang"),
        QStringLiteral("threesome"), QStringLiteral("orgy"), QStringLiteral("uncensored"),
        QStringLiteral("censored"), QStringLiteral("mosaic_censoring"), QStringLiteral("bar_censor"),
    };
    return tags;
}

// NOTE: "breasts"/"nipples"/"areola" are deliberately NOT in the defaults —
// they appear on SFW posts too (e.g. clothed emphasis tagged "breasts"
// rated g). Add them in Preferences if you want them flagged.

namespace ExplicitDetail {
inline QMutex &mutex() {
    static QMutex m;
    return m;
}
inline QSet<QString> &custom() {
    static QSet<QString> s;
    return s;
}
inline bool &hasCustom() {
    static bool b = false;
    return b;
}
} // namespace ExplicitDetail

// The set scanners actually match against (defaults until replaced).
inline QSet<QString> activeExplicitTags() {
    QMutexLocker lock(&ExplicitDetail::mutex());
    if (ExplicitDetail::hasCustom())
        return ExplicitDetail::custom();
    return defaultExplicitTags();
}

// Replace the active set (e.g. from Preferences). Lowercase exact-match
// semantics are applied by the caller contract; stored as given.
inline void setActiveExplicitTags(const QSet<QString> &tags) {
    QMutexLocker lock(&ExplicitDetail::mutex());
    ExplicitDetail::custom() = tags;
    ExplicitDetail::hasCustom() = true;
}

// Canonical form for the editor/settings: lowercase, trimmed, de-duped.
inline QStringList normalizeExplicitTags(const QStringList &tags) {
    QSet<QString> seen;
    for (const QString &t : tags) {
        const QString n = t.trimmed().toLower();
        if (!n.isEmpty())
            seen.insert(n);
    }
    QStringList out(seen.begin(), seen.end());
    out.sort();
    return out;
}

inline QStringList defaultExplicitTagList() {
    QStringList out(defaultExplicitTags().begin(), defaultExplicitTags().end());
    out.sort();
    return out;
}

// Hash baked into the scan cache: editing the list invalidates old ratings.
inline QString explicitTagsHash() {
    const QStringList sorted =
        normalizeExplicitTags(QStringList(activeExplicitTags().begin(),
                                          activeExplicitTags().end()));
    return QString::fromLatin1(
        QCryptographicHash::hash(sorted.join(u'\n').toUtf8(),
                                 QCryptographicHash::Sha1)
            .toHex()
            .left(16));
}

// Returns {rating, inferred}. Exact, case-insensitive tag match.
inline std::pair<QString, bool> inferExplicit(const QStringList &allTags, const QString &rating) {
    if (rating == QStringLiteral("explicit"))
        return {rating, false};
    const QSet<QString> et = activeExplicitTags();
    for (const QString &t : allTags) {
        if (et.contains(t.trimmed().toLower()))
            return {QStringLiteral("explicit"), true};
    }
    return {rating, false};
}

inline QString normalizeRating(const QString &v) {
    const QString s = v.trimmed().toLower();
    if (s == QStringLiteral("g") || s == QStringLiteral("general"))
        return QStringLiteral("general");
    if (s == QStringLiteral("s") || s == QStringLiteral("sensitive"))
        return QStringLiteral("sensitive");
    if (s == QStringLiteral("q") || s == QStringLiteral("questionable"))
        return QStringLiteral("questionable");
    if (s == QStringLiteral("e") || s == QStringLiteral("explicit"))
        return QStringLiteral("explicit");
    return QStringLiteral("general");
}
