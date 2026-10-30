#pragma once

// Booru-specific tag-category merging/renaming (Preferences → Tags →
// "Merge tag categories"). Native taxonomies are inconsistent: Soybooru
// sends `character`/`variant`, some hosts repeat `meta`, meme boorus use
// their own names. A rule groups one or more category names from one
// booru (or all boorus) under a single display target.
//
// Target forms:
//   - a classic bucket alias (artist(s), character(s), copyright/ip,
//     meta/metadata, general) → tags join that bucket and the section
//     label becomes the typed target;
//   - anything else (e.g. `soybooru_specific`) → one custom group.
//
// Sources are category names as the booru sends them (case-insensitive,
// singular/plural loose). `*` matches every category.
#include <QList>
#include <QString>
#include <QStringList>

struct TagCategoryMerge {
    QString siteKey; // empty = every booru; otherwise a configured entry key
    QStringList sources;
    QString target;

    bool operator==(const TagCategoryMerge &) const = default;
};

namespace TagCategories {

inline QString normalizeKey(QString s) {
    s = s.trimmed().toLower();
    s.replace(u'_', u' ');
    s.replace(u'-', u' ');
    return s.simplified();
}

// Classic bucket for a category/target alias ("" = genuinely custom).
inline QString classicBucket(const QString &category) {
    const QString c = normalizeKey(category);
    if (c.isEmpty())
        return {};
    if (c == QStringLiteral("artist") || c == QStringLiteral("artists") ||
        c == QStringLiteral("creator") || c == QStringLiteral("creators"))
        return QStringLiteral("artist");
    if (c == QStringLiteral("character") ||
        c == QStringLiteral("characters") || c == QStringLiteral("char") ||
        c == QStringLiteral("chars") || c == QStringLiteral("variant") ||
        c == QStringLiteral("variants") || c == QStringLiteral("subvariant") ||
        c == QStringLiteral("subvariants"))
        return QStringLiteral("character");
    if (c == QStringLiteral("copyright") ||
        c == QStringLiteral("copyrights") || c == QStringLiteral("ip") ||
        c == QStringLiteral("series") || c == QStringLiteral("franchise") ||
        c == QStringLiteral("franchises"))
        return QStringLiteral("copyright");
    if (c == QStringLiteral("meta") || c == QStringLiteral("metas") ||
        c == QStringLiteral("metadata") || c == QStringLiteral("medium") ||
        c == QStringLiteral("media") || c == QStringLiteral("title") ||
        c == QStringLiteral("titles"))
        return QStringLiteral("meta");
    if (c == QStringLiteral("general") || c == QStringLiteral("generals") ||
        c == QStringLiteral("uncategorized") || c == QStringLiteral("default"))
        return QStringLiteral("general");
    return {};
}

inline bool scopeMatches(const TagCategoryMerge &m, const QString &siteKey) {
    return m.siteKey.isEmpty() ||
           m.siteKey.compare(siteKey, Qt::CaseInsensitive) == 0;
}

inline bool sourceMatches(const TagCategoryMerge &m, const QString &category) {
    const QString cat = normalizeKey(category);
    for (const QString &raw : m.sources) {
        const QString s = normalizeKey(raw);
        if (s == QStringLiteral("*") || s == cat)
            return true;
        // Singular/plural aliases: `characters` also matches `character`.
        if (!classicBucket(s).isEmpty() &&
            classicBucket(s) == classicBucket(cat))
            return true;
    }
    return false;
}

inline const TagCategoryMerge *
mergeFor(const QList<TagCategoryMerge> &rules, const QString &siteKey,
         const QString &category) {
    for (const TagCategoryMerge &m : rules) {
        if (scopeMatches(m, siteKey) && sourceMatches(m, category) &&
            !m.target.trimmed().isEmpty())
            return &m;
    }
    return nullptr;
}

// Section label for a classic bucket after renames (empty = default).
inline QString labelFor(const QList<TagCategoryMerge> &rules,
                        const QString &siteKey, const QString &bucket) {
    for (const TagCategoryMerge &m : rules) {
        if (!scopeMatches(m, siteKey) || m.target.trimmed().isEmpty())
            continue;
        if (classicBucket(m.target) != bucket)
            continue;
        if (sourceMatches(m, bucket))
            return m.target.trimmed();
    }
    return {};
}

inline QString displayTarget(const QString &target) {
    QString label = target.trimmed();
    label.replace(u'_', u' ');
    label = label.simplified();
    if (!label.isEmpty())
        label[0] = label[0].toUpper();
    return label;
}

// Stable key for a custom merged group (classic targets return the bucket).
inline QString targetKey(const QString &target) {
    const QString bucket = classicBucket(target);
    return bucket.isEmpty() ? normalizeKey(target) : bucket;
}

// "scope|src1,src2,src3|Target" settings.ini lines.
inline QList<TagCategoryMerge>
fromLines(const QStringList &lines) {
    QList<TagCategoryMerge> out;
    for (const QString &line : lines) {
        const QStringList parts = line.split(u'|');
        if (parts.size() < 3)
            continue;
        TagCategoryMerge m;
        m.siteKey = parts.at(0).trimmed();
        for (const QString &src : parts.at(1).split(u',')) {
            const QString s = src.trimmed();
            if (!s.isEmpty())
                m.sources << s;
        }
        m.target = parts.at(2).trimmed();
        if (!m.sources.isEmpty() && !m.target.isEmpty())
            out.append(m);
    }
    return out;
}

inline QStringList toLines(const QList<TagCategoryMerge> &rules) {
    QStringList out;
    for (const TagCategoryMerge &m : rules) {
        if (m.sources.isEmpty() || m.target.trimmed().isEmpty())
            continue;
        out << m.siteKey + u'|' + m.sources.join(u',') + u'|' +
                   m.target.trimmed();
    }
    return out;
}

} // namespace TagCategories
