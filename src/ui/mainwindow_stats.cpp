// Info -> Statistics…: gather the database side on the UI thread and the
// filesystem side (download folder + cache trees) on a worker, then let
// the popup merge both. Favourites are deliberately not re-read from the
// archive scan: only the DB knows the full set.
#include "mainwindow.h"
#include "settings.h"
#include "statsdialog.h"
#include "tagcountcache.h"

#include <QDir>
#include <QFutureWatcher>
#include <QPointer>
#include <QtConcurrent>
#include <algorithm>

namespace {

// Resolve site keys/labels on the UI thread so the worker never reads the
// static booru registry (Preferences can swap it while the scan runs).
void fillStatsSiteMaps(StatsScanInput &input, const QList<BooruSite> &sites) {
    for (const BooruSite &site : sites) {
        if (site.key.isEmpty())
            continue;
        if (!site.label.isEmpty()) {
            input.keyLabels.insert(site.key, site.label);
            input.labelKeys.insert(site.label.trimmed().toLower(), site.key);
        }
        input.labelKeys.insert(site.key.toLower(), site.key);
        if (!site.shortName.isEmpty())
            input.labelKeys.insert(site.shortName.trimmed().toLower(),
                                  site.key);
    }
    const QDir root(input.downloadDir);
    for (const QString &folder :
         root.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        QString host = folder;
        const qsizetype us = host.lastIndexOf(u'_');
        if (us > 0) {
            bool port = false;
            host.mid(us + 1).toInt(&port);
            if (port)
                host = host.left(us);
        }
        const BooruSite *entry = RemotePosts::entryForHost(host);
        if (!entry) {
            const QString key = RemotePosts::keyForLabel(folder);
            if (!key.isEmpty())
                entry = RemoteSource::siteFor(key);
        }
        if (entry) {
            input.folderKeys.insert(folder, entry->key);
            input.folderLabels.insert(folder, entry->label);
        } else {
            input.folderLabels.insert(folder, folder);
        }
    }
}

} // namespace

void MainWindow::showStatistics() {
    const QVector<Post> favourites =
        m_favDb.list(qMax(1, m_favDb.count()));

    StatsReport report = statsFromFavourites(favourites);
    report.cachedTagRows = m_tagCache.size();
    report.cacheLimitMB = m_settings.cacheLimitMB;

    // The archive open in this tab, when it is a plain local archive.
    // Favourites/downloads tabs would duplicate the library numbers.
    BrowserTab *tab = currentTab();
    const QString downloadRoot =
        QDir(effectiveDownloadDir(m_settings)).absolutePath();
    if (tab && !tab->isRemote && !tab->isFavourites &&
        QDir(tab->root).absolutePath() != downloadRoot) {
        const Stats archive = m_index.stats();
        report.archive.posts = archive.total;
        report.archive.ratings = archive.ratings;
        QHash<QString, int> top;
        for (auto bucket = m_index.tagCounts().constBegin();
             bucket != m_index.tagCounts().constEnd(); ++bucket) {
            for (auto tag = bucket.value().constBegin();
                 tag != bucket.value().constEnd(); ++tag) {
                // A tag should live in one bucket; max (not sum) keeps a
                // stale duplicate from inflating the post count.
                top[tag.key()] = qMax(top.value(tag.key()), tag.value());
            }
        }
        report.archive.topTags = statsTopEntries(top, 20);
    }

    auto *dialog = new StatsDialog(report, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->show();

    StatsScanInput input;
    input.downloadDir = effectiveDownloadDir(m_settings);
    input.cacheDir = appCacheDir();
    input.favouriteIds = statsPostKeys(favourites);
    QList<BooruSite> knownSites = RemoteSource::presetBoorus();
    knownSites += m_settings.boorus; // configured entries win collisions
    fillStatsSiteMaps(input, knownSites);

    auto *watcher = new QFutureWatcher<StatsScanResult>(this);
    const QPointer<StatsDialog> guard(dialog);
    connect(watcher, &QFutureWatcher<StatsScanResult>::finished, this,
            [guard, watcher] {
                if (guard)
                    guard->mergeScan(watcher->result());
                watcher->deleteLater();
            });
    watcher->setFuture(QtConcurrent::run(
        [input] { return scanStatsPaths(input); }));
}
