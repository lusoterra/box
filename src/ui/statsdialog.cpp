#include "statsdialog.h"

#include "explicittags.h"
#include "metaparser.h"
#include "remotesource.h"

#include <QDir>
#include <QDirIterator>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QPalette>
#include <QTextBrowser>
#include <QVBoxLayout>

#include <algorithm>

namespace {

QString esc(const QString &s) {
    return s.toHtmlEscaped();
}

QString humanBytes(qint64 bytes) {
    static const char *units[] = {"B", "KB", "MB", "GB", "TB"};
    double v = double(qMax<qint64>(0, bytes));
    int unit = 0;
    while (v >= 1024.0 && unit < 4) {
        v /= 1024.0;
        ++unit;
    }
    return QStringLiteral("%1 %2")
        .arg(QString::number(v, 'f', unit == 0 ? 0 : 1),
             QString::fromLatin1(units[unit]));
}

QString countText(qint64 n) {
    return QLocale().toString(n);
}

bool isMediaExt(const QString &ext) {
    static const QSet<QString> exts = [] {
        QSet<QString> out;
        for (const QString &e : stillImageExts())
            out.insert(e);
        for (const QString &e : gifLikeExts())
            out.insert(e);
        for (const QString &e : videoExts())
            out.insert(e);
        out.insert(QStringLiteral("jpe"));
        out.insert(QStringLiteral("jfif"));
        return out;
    }();
    return exts.contains(ext);
}

void addBlob(QHash<QString, int> &dst, const QString &blob) {
    for (const QString &tag :
         cleanTagTokens(blob.split(u' ', Qt::SkipEmptyParts)))
        dst[tag.toLower()]++;
}

void addPostTags(const Post &p, QHash<QString, int> &all,
                 QHash<QString, int> &artists,
                 QHash<QString, int> &characters,
                 QHash<QString, int> &copyrights) {
    addBlob(all, p.tags);
    // Fallback for rows whose combined blob is empty but the buckets are
    // not (hand-built POSTs, partial imports).
    if (p.tags.trimmed().isEmpty()) {
        addBlob(all, p.tagsArtist);
        addBlob(all, p.tagsCharacter);
        addBlob(all, p.tagsCopyright);
        addBlob(all, p.tagsGeneral);
        addBlob(all, p.tagsMeta);
    }
    addBlob(artists, p.tagsArtist);
    addBlob(characters, p.tagsCharacter);
    addBlob(copyrights, p.tagsCopyright);
}

struct SiteRef {
    QString key;
    QString label;
};

SiteRef siteFromLabel(const QString &label) {
    const QString key = RemotePosts::keyForLabel(label.trimmed());
    if (!key.isEmpty()) {
        if (const BooruSite *site = RemoteSource::siteFor(key))
            return {site->key, site->label};
    }
    return {QString(), label.trimmed()};
}

// Worker-side label resolution: only the maps snapshotted on the UI
// thread, never the static registry.
SiteRef labelSiteFromInput(const StatsScanInput &in, const QString &label) {
    const QString clean = label.trimmed();
    if (clean.isEmpty())
        return {};
    const QString key = in.labelKeys.value(clean.toLower());
    if (!key.isEmpty()) {
        const QString display = in.keyLabels.value(key);
        return {key, display.isEmpty() ? clean : display};
    }
    return {QString(), clean};
}

// Folder names are host tags ("danbooru.donmai.us"), legacy labels
// ("Danbooru") or "_Stray"; flat legacy layouts prefix the filename.
SiteRef folderSiteFromInput(const StatsScanInput &in, const QString &folder,
                            const QString &fileName, bool nested) {
    if (nested && !folder.isEmpty()) {
        const QString key = in.folderKeys.value(folder);
        if (!key.isEmpty()) {
            QString label = in.folderLabels.value(folder);
            if (label.isEmpty())
                label = in.keyLabels.value(key, key);
            return {key, label};
        }
        const QString label = in.folderLabels.value(folder);
        if (!label.isEmpty())
            return {QString(), label};
        return labelSiteFromInput(in, folder);
    }
    QString stem = fileName;
    const qsizetype dot = stem.lastIndexOf(u'.');
    if (dot > 0)
        stem = stem.left(dot);
    const qsizetype us = stem.lastIndexOf(u'_');
    if (us > 0 && isMd5Hex(stem.mid(us + 1)))
        stem = stem.left(us);
    return labelSiteFromInput(in, stem);
}

SiteRef siteFromPost(const Post &p, const SiteRef &fallback = {}) {
    QString key = p.siteKey;
    if (key.isEmpty() && !p.siteName.isEmpty())
        key = RemotePosts::keyForLabel(p.siteName);
    if (!key.isEmpty()) {
        if (const BooruSite *site = RemoteSource::siteFor(key))
            return {site->key, site->label};
        return {key, p.siteName.isEmpty() ? key : p.siteName};
    }
    if (!p.siteName.isEmpty() && p.siteName != QStringLiteral("local") &&
        p.siteName != QStringLiteral("_Stray"))
        return {QString(), p.siteName};
    if (!p.siteName.isEmpty() && p.siteName == QStringLiteral("_Stray"))
        return {QString(), p.siteName};
    return fallback;
}

QString postKeyFor(const Post &p, const SiteRef &fallback = {}) {
    const SiteRef site = siteFromPost(p, fallback);
    if (p.id > 0 && !site.key.isEmpty())
        return site.key + u'#' + QString::number(p.id);
    if (!p.fileUrl.isEmpty())
        return QStringLiteral("url:") + p.fileUrl;
    if (!p.postUrl.isEmpty())
        return QStringLiteral("url:") + p.postUrl;
    if (!p.absFile.isEmpty())
        return QStringLiteral("path:") + p.absFile;
    return {};
}

SiteRef postSiteFromInput(const StatsScanInput &in, const Post &p,
                          const SiteRef &fallback = {}) {
    QString key = p.siteKey;
    if (key.isEmpty() && !p.siteName.isEmpty())
        key = in.labelKeys.value(p.siteName.trimmed().toLower());
    if (!key.isEmpty()) {
        QString label = in.keyLabels.value(key);
        if (label.isEmpty())
            label = p.siteName.isEmpty() ? key : p.siteName;
        return {key, label};
    }
    if (!p.siteName.isEmpty() && p.siteName != QStringLiteral("local"))
        return {QString(), p.siteName};
    return fallback;
}

QString postKeyFromInput(const StatsScanInput &in, const Post &p,
                         const SiteRef &fallback = {}) {
    const SiteRef site = postSiteFromInput(in, p, fallback);
    if (p.id > 0 && !site.key.isEmpty())
        return site.key + u'#' + QString::number(p.id);
    if (!p.fileUrl.isEmpty())
        return QStringLiteral("url:") + p.fileUrl;
    if (!p.postUrl.isEmpty())
        return QStringLiteral("url:") + p.postUrl;
    if (!p.absFile.isEmpty())
        return QStringLiteral("path:") + p.absFile;
    return {};
}

Post postFromSidecar(const QString &path, bool *ok) {
    *ok = false;
    QFile f(path);
    // Sidecars are KBs; mirror the scanner's bounded read.
    static constexpr qint64 kMaxSidecar = 8LL * 1024 * 1024;
    if (f.size() > kMaxSidecar || !f.open(QIODevice::ReadOnly))
        return {};
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
        return {};
    const QJsonObject raw = doc.object();
    Post p;
    if (!MetaParser::parse(raw, p))
        return p;
    // MetaParser does not carry media/post URLs; read them so a
    // downloaded sidecar can still match its favourites-row twin when the
    // host no longer resolves to a configured entry.
    if (p.fileUrl.isEmpty()) {
        for (const char *key : {"file_url", "fileUrl", "url"}) {
            const QString v = raw.value(QLatin1String(key)).toString();
            if (!v.isEmpty()) {
                p.fileUrl = v;
                break;
            }
        }
    }
    if (p.postUrl.isEmpty()) {
        for (const char *key : {"post_url", "postUrl"}) {
            const QString v = raw.value(QLatin1String(key)).toString();
            if (!v.isEmpty()) {
                p.postUrl = v;
                break;
            }
        }
    }
    *ok = true;
    return p;
}

void mergeCounts(QHash<QString, int> &dst, const QHash<QString, int> &src) {
    for (auto it = src.constBegin(); it != src.constEnd(); ++it)
        dst[it.key()] += it.value();
}

} // namespace

StatsReport statsFromFavourites(const QVector<Post> &favourites) {
    StatsReport r;
    for (const Post &p : favourites) {
        ++r.favourites;
        addPostTags(p, r.tagCounts, r.artistCounts, r.characterCounts,
                    r.copyrightCounts);
        const SiteRef site = siteFromPost(p);
        if (!site.label.isEmpty())
            r.booruCounts[site.label]++;
        r.ratingCounts[normalizeRating(p.rating)]++;
    }
    r.libraryPosts = r.favourites;
    return r;
}

QSet<QString> statsPostKeys(const QVector<Post> &posts) {
    QSet<QString> keys;
    for (const Post &p : posts) {
        const QString key = postKeyFor(p);
        if (!key.isEmpty())
            keys.insert(key);
        // DNS aliases: an unconfigured booru cannot resolve host -> key,
        // so the media/post URLs still let a downloaded favourite match.
        if (!p.fileUrl.isEmpty())
            keys.insert(QStringLiteral("url:") + p.fileUrl);
        if (!p.postUrl.isEmpty())
            keys.insert(QStringLiteral("url:") + p.postUrl);
    }
    return keys;
}

QVector<StatEntry> statsTopEntries(const QHash<QString, int> &counts,
                                   int limit) {
    QVector<StatEntry> out;
    out.reserve(counts.size());
    for (auto it = counts.constBegin(); it != counts.constEnd(); ++it)
        out.append({it.key(), it.value()});
    std::sort(out.begin(), out.end(), [](const StatEntry &a, const StatEntry &b) {
        if (a.count != b.count)
            return a.count > b.count;
        return QString::compare(a.label, b.label, Qt::CaseInsensitive) < 0;
    });
    if (out.size() > limit)
        out.resize(limit);
    return out;
}

StatsScanResult scanStatsPaths(const StatsScanInput &input) {
    StatsScanResult r;

    // ── Cache tree: thumbs/, full/, roots/ + anything else (favicons,
    // session temp dirs) count toward the total only. ──
    if (!input.cacheDir.isEmpty() && QDir(input.cacheDir).exists()) {
        r.cacheScanned = true;
        const QDir root(input.cacheDir);
        QDirIterator it(input.cacheDir,
                        QDir::Files | QDir::NoDotAndDotDot | QDir::Hidden,
                        QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString path = it.next();
            const qint64 size = it.fileInfo().size();
            r.cacheBytes += size;
            const QString top =
                root.relativeFilePath(path).section(u'/', 0, 0);
            if (top == QStringLiteral("thumbs")) {
                r.thumbBytes += size;
                ++r.thumbFiles;
            } else if (top == QStringLiteral("full")) {
                r.fullBytes += size;
                ++r.fullFiles;
            } else if (top == QStringLiteral("roots")) {
                r.indexBytes += size;
                ++r.indexFiles;
            }
        }
    }

    // ── Download folder: media files, sidecar tags, site counts. ──
    if (input.downloadDir.isEmpty() || !QDir(input.downloadDir).exists())
        return r;
    QSet<QString> seen = input.favouriteIds;
    const QDir root(input.downloadDir);
    QDirIterator it(input.downloadDir,
                    QDir::Files | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        const QFileInfo fi(path);
        const QString ext = fi.suffix().toLower();
        if (ext == QStringLiteral("json") || !isMediaExt(ext))
            continue;

        ++r.downloadedFiles;
        r.downloadedBytes += fi.size();
        if (videoExts().contains(ext))
            ++r.videos;
        else if (gifLikeExts().contains(ext))
            ++r.gifs;
        else
            ++r.images;

        const QString rel = root.relativeFilePath(path);
        const bool nested = rel.contains(u'/');
        const QString folder = nested ? rel.section(u'/', 0, 0) : QString();
        const SiteRef folderSite = folderSiteFromInput(
            input, folder, fi.fileName(), nested);

        Post post;
        bool havePost = false;
        const QStringList candidates = {
            fi.absolutePath() + u'/' + fi.completeBaseName() +
                QStringLiteral(".json"),
            path + QStringLiteral(".json")};
        for (const QString &sidecar : candidates) {
            if (!QFileInfo::exists(sidecar))
                continue;
            bool ok = false;
            Post parsed = postFromSidecar(sidecar, &ok);
            if (ok) {
                post = parsed;
                havePost = true;
            }
            break;
        }

        const SiteRef site =
            havePost ? postSiteFromInput(input, post, folderSite) : folderSite;
        QString key = havePost ? postKeyFromInput(input, post, site) : QString();
        if (key.isEmpty())
            key = QStringLiteral("path:") + fi.absoluteFilePath();
        // A downloaded favourite already contributed its tags/site below;
        // do not count the same post twice. The file still counts above.
        const bool alreadyCounted =
            seen.contains(key) ||
            (havePost && !post.fileUrl.isEmpty() &&
             seen.contains(QStringLiteral("url:") + post.fileUrl)) ||
            (havePost && !post.postUrl.isEmpty() &&
             seen.contains(QStringLiteral("url:") + post.postUrl));
        if (alreadyCounted)
            continue;
        seen.insert(key);
        if (havePost && !post.fileUrl.isEmpty())
            seen.insert(QStringLiteral("url:") + post.fileUrl);
        if (havePost && !post.postUrl.isEmpty())
            seen.insert(QStringLiteral("url:") + post.postUrl);

        ++r.downloadedPosts;
        if (!site.label.isEmpty())
            r.booruCounts[site.label]++;
        if (havePost) {
            addPostTags(post, r.tagCounts, r.artistCounts, r.characterCounts,
                        r.copyrightCounts);
            r.ratingCounts[normalizeRating(post.rating)]++;
        }
    }
    return r;
}

StatsDialog::StatsDialog(const StatsReport &report, QWidget *parent)
    : QDialog(parent), m_report(report) {
    setWindowTitle(tr("Statistics"));
    resize(760, 720);

    m_view = new QTextBrowser(this);
    m_view->setOpenExternalLinks(false);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);

    auto *lay = new QVBoxLayout(this);
    lay->addWidget(m_view, 1);
    lay->addWidget(buttons);

    rebuild();
}

void StatsDialog::mergeScan(const StatsScanResult &scan) {
    m_report.downloadedFiles = scan.downloadedFiles;
    m_report.downloadedPosts = scan.downloadedPosts;
    m_report.downloadedBytes = scan.downloadedBytes;
    m_report.images = scan.images;
    m_report.gifs = scan.gifs;
    m_report.videos = scan.videos;
    mergeCounts(m_report.tagCounts, scan.tagCounts);
    mergeCounts(m_report.artistCounts, scan.artistCounts);
    mergeCounts(m_report.characterCounts, scan.characterCounts);
    mergeCounts(m_report.copyrightCounts, scan.copyrightCounts);
    mergeCounts(m_report.ratingCounts, scan.ratingCounts);
    mergeCounts(m_report.booruCounts, scan.booruCounts);
    m_report.cacheScanned = scan.cacheScanned;
    m_report.cacheBytes = scan.cacheBytes;
    m_report.thumbBytes = scan.thumbBytes;
    m_report.fullBytes = scan.fullBytes;
    m_report.indexBytes = scan.indexBytes;
    m_report.thumbFiles = scan.thumbFiles;
    m_report.fullFiles = scan.fullFiles;
    m_report.indexFiles = scan.indexFiles;
    m_report.libraryPosts = m_report.favourites + scan.downloadedPosts;
    m_scanning = false;
    rebuild();
}

void StatsDialog::rebuild() {
    m_view->setHtml(buildHtml());
}

QString StatsDialog::buildHtml() const {
    const QPalette pal = palette();
    const QString textColor = pal.color(QPalette::WindowText).name();
    const QString mutedColor =
        pal.color(QPalette::Disabled, QPalette::WindowText).name();
    const QString cardColor = pal.color(QPalette::Base).name();
    const QString altColor = pal.color(QPalette::AlternateBase).name();
    const QString accentColor = pal.color(QPalette::Highlight).name();

    const auto card = [&](const QString &value, const QString &label,
                          const QString &sub = QString()) {
        QString note = esc(label);
        if (!sub.isEmpty())
            note += QStringLiteral("<br><span style=\"color:%1\">%2</span>")
                        .arg(mutedColor, esc(sub));
        return QStringLiteral(
                   "<td width=\"25%\" bgcolor=\"%1\" style=\"padding:10px\">"
                   "<div style=\"font-size:16pt\"><b><font color=\"%2\">%3"
                   "</font></b></div>"
                   "<div style=\"font-size:8pt\"><font color=\"%4\">%5"
                   "</font></div></td>")
            .arg(cardColor, accentColor, esc(value), mutedColor, note);
    };

    QString html;
    html += QStringLiteral(
                "<html><body style=\"color:%1\">"
                "<h2 style=\"margin:0 0 2px 0\">%2</h2>"
                "<div style=\"font-size:9pt\"><font color=\"%3\">%4</font>"
                "</div>")
                .arg(textColor, tr("Library statistics"),
                     mutedColor,
                     tr("A snapshot of what Box has collected on this "
                        "machine."));

    if (m_scanning) {
        html += QStringLiteral(
                    "<p><i><font color=\"%1\">%2</font></i></p>")
                    .arg(mutedColor,
                         tr("Reading the download folder and cache…"));
    }

    const auto section = [&](const QString &title) {
        return QStringLiteral("<h3 style=\"margin:18px 0 6px 0\">%1</h3>")
            .arg(esc(title));
    };
    const auto cards = [&](const QVector<QString> &cells) {
        QString out = QStringLiteral(
            "<table width=\"100%\" cellspacing=\"6\" cellpadding=\"0\">");
        for (qsizetype i = 0; i < cells.size(); i += 4) {
            out += QStringLiteral("<tr>");
            for (qsizetype j = i; j < i + 4; ++j)
                out += j < cells.size() ? cells.at(j)
                                        : QStringLiteral("<td width=\"25%\">"
                                                         "</td>");
            out += QStringLiteral("</tr>");
        }
        return out + QStringLiteral("</table>");
    };

    // While the worker walks the disk, download/cache numbers stay "—"
    // instead of showing a misleading 0 that jumps a second later.
    const QString pending = QStringLiteral("—");

    // ── Library ──
    QVector<QString> lib;
    lib << card(countText(m_report.favourites), tr("Favourited posts"));
    lib << card(m_scanning ? pending : countText(m_report.downloadedFiles),
                tr("Downloaded files"),
                !m_scanning && m_report.downloadedBytes > 0
                    ? humanBytes(m_report.downloadedBytes)
                    : QString());
    lib << card(m_scanning ? pending : countText(m_report.libraryPosts),
                tr("Unique posts in library"),
                m_scanning
                    ? QString()
                    : tr("%1 favourites + %2 downloads")
                          .arg(countText(m_report.favourites),
                               countText(m_report.downloadedPosts)));
    lib << card(m_report.cacheScanned && !m_scanning
                    ? humanBytes(m_report.cacheBytes)
                    : pending,
                tr("Cache size"),
                m_report.cacheLimitMB > 0
                    ? tr("limit %1 MB").arg(m_report.cacheLimitMB)
                    : QString());
    html += section(tr("Library"));
    html += cards(lib);

    // ── Media mix ──
    QVector<QString> mix;
    mix << card(m_scanning ? pending : countText(m_report.images),
                tr("Images"));
    mix << card(m_scanning ? pending : countText(m_report.gifs),
                tr("GIFs / animated"));
    mix << card(m_scanning ? pending : countText(m_report.videos),
                tr("Videos"));
    mix << card(countText(m_report.tagCounts.size()), tr("Unique tags"));
    html += section(tr("Media mix"));
    html += cards(mix);

    // ── Cache ──
    QVector<QString> cache;
    cache << card(m_scanning ? pending : countText(m_report.thumbFiles),
                  tr("Cached thumbnails"),
                  m_report.cacheScanned && !m_scanning
                      ? humanBytes(m_report.thumbBytes)
                      : QString());
    cache << card(m_scanning ? pending : countText(m_report.fullFiles),
                  tr("Cached full images"),
                  m_report.cacheScanned && !m_scanning
                      ? humanBytes(m_report.fullBytes)
                      : QString());
    cache << card(m_scanning ? pending : countText(m_report.indexFiles),
                  tr("Scan index files"),
                  m_report.cacheScanned && !m_scanning
                      ? humanBytes(m_report.indexBytes)
                      : QString());
    cache << card(countText(m_report.cachedTagRows),
                  tr("Tags cached for suggestions"));
    html += section(tr("Cache + tag vocabulary"));
    html += cards(cache);

    if (!m_scanning && m_report.favourites == 0 &&
        m_report.downloadedFiles == 0 && m_report.archive.posts == 0) {
        html += QStringLiteral(
                    "<p><i><font color=\"%1\">%2</font></i></p>")
                    .arg(mutedColor,
                         tr("Nothing collected yet - favourite or download "
                            "posts and the numbers will show up here."));
    }

    // ── Most common tags ──
    const QVector<StatEntry> tags = statsTopEntries(m_report.tagCounts, 30);
    if (!tags.isEmpty()) {
        html += section(tr("Most common tags"));
        html += QStringLiteral(
            "<table width=\"100%\" cellspacing=\"0\" cellpadding=\"3\">");
        for (qsizetype i = 0; i < tags.size(); ++i) {
            const QString bg = (i % 2) ? altColor : cardColor;
            html += QStringLiteral(
                        "<tr bgcolor=\"%1\"><td>%2</td>"
                        "<td align=\"right\"><font color=\"%3\">%4</font>"
                        "</td></tr>")
                        .arg(bg, esc(tags.at(i).label), mutedColor,
                             tr("%1 posts").arg(countText(tags.at(i).count)));
        }
        html += QStringLiteral("</table>");
    }

    // ── Creative breakdown: artists, characters, series. ──
    const auto rankedTable = [&](const QString &title,
                                 const QVector<StatEntry> &entries) {
        if (entries.isEmpty())
            return QString();
        QString out = section(title);
        out += QStringLiteral(
            "<table width=\"100%\" cellspacing=\"0\" cellpadding=\"3\">");
        for (qsizetype i = 0; i < entries.size(); ++i) {
            const QString bg = (i % 2) ? altColor : cardColor;
            out += QStringLiteral(
                       "<tr bgcolor=\"%1\"><td>%2</td>"
                       "<td align=\"right\"><font color=\"%3\">%4</font></td>"
                       "</tr>")
                       .arg(bg, esc(entries.at(i).label), mutedColor,
                            countText(entries.at(i).count));
        }
        out += QStringLiteral("</table>");
        return out;
    };
    html += rankedTable(tr("Top artists"),
                        statsTopEntries(m_report.artistCounts, 15));
    html += rankedTable(tr("Top characters"),
                        statsTopEntries(m_report.characterCounts, 15));
    html += rankedTable(tr("Top series"),
                        statsTopEntries(m_report.copyrightCounts, 15));

    // ── Booru ranking ──
    const QVector<StatEntry> boorus = statsTopEntries(m_report.booruCounts, 15);
    if (!boorus.isEmpty()) {
        html += section(tr("Most used boorus"));
        html += QStringLiteral(
            "<table width=\"100%\" cellspacing=\"0\" cellpadding=\"3\">");
        for (qsizetype i = 0; i < boorus.size(); ++i) {
            const int pct =
                m_report.libraryPosts > 0
                    ? int(100.0 * boorus.at(i).count / m_report.libraryPosts +
                          0.5)
                    : 0;
            const QString bg = (i % 2) ? altColor : cardColor;
            html += QStringLiteral(
                        "<tr bgcolor=\"%1\"><td width=\"6%\"><font color=\"%2\">"
                        "%3</font></td><td>%4</td>"
                        "<td align=\"right\"><font color=\"%2\">%5</font>"
                        "</td></tr>")
                        .arg(bg, mutedColor, countText(i + 1),
                             esc(boorus.at(i).label),
                             tr("%1 posts · %2%")
                                 .arg(countText(boorus.at(i).count),
                                      QString::number(pct)));
        }
        html += QStringLiteral("</table>");
    }

    // ── Ratings ──
    const QStringList ratingOrder = {QStringLiteral("general"),
                                     QStringLiteral("sensitive"),
                                     QStringLiteral("questionable"),
                                     QStringLiteral("explicit")};
    QVector<QString> ratings;
    for (const QString &rating : ratingOrder) {
        const int n = m_report.ratingCounts.value(rating);
        if (n == 0)
            continue;
        QString label;
        if (rating == QStringLiteral("general"))
            label = tr("General");
        else if (rating == QStringLiteral("sensitive"))
            label = tr("Sensitive");
        else if (rating == QStringLiteral("questionable"))
            label = tr("Questionable");
        else
            label = tr("Explicit");
        ratings << card(countText(n), label);
    }
    if (!ratings.isEmpty()) {
        html += section(tr("Ratings"));
        html += cards(ratings);
    }

    // ── Open archive (only for a plain local tab). ──
    if (m_report.archive.posts > 0) {
        html += section(tr("Open archive"));
        QVector<QString> archive;
        archive << card(countText(m_report.archive.posts), tr("Scanned posts"));
        int general = 0;
        int explicitPosts = 0;
        for (auto it = m_report.archive.ratings.constBegin();
             it != m_report.archive.ratings.constEnd(); ++it) {
            if (it.key() == QStringLiteral("general"))
                general += it.value();
            else if (it.key() == QStringLiteral("explicit"))
                explicitPosts += it.value();
        }
        archive << card(countText(general), tr("General rated"));
        archive << card(countText(explicitPosts), tr("Explicit rated"));
        html += cards(archive);
        if (!m_report.archive.topTags.isEmpty()) {
            html += QStringLiteral(
                "<table width=\"100%\" cellspacing=\"0\" cellpadding=\"3\">");
            const auto &top = m_report.archive.topTags;
            for (qsizetype i = 0; i < top.size(); ++i) {
                const QString bg =
                    (i % 2) ? altColor : cardColor;
                html += QStringLiteral(
                            "<tr bgcolor=\"%1\"><td>%2</td>"
                            "<td align=\"right\"><font color=\"%3\">%4</font>"
                            "</td></tr>")
                            .arg(bg, esc(top.at(i).label), mutedColor,
                                 tr("%1 posts").arg(countText(top.at(i).count)));
            }
            html += QStringLiteral("</table>");
        }
    }

    html += QStringLiteral("</body></html>");
    return html;
}
