#include "mainwindow.h"
#include "appinfo.h"
#include "cache.h"
#include "cachejanitor.h"
#include "cachepolicy.h"
#include "cookiegrab.h"
#include "downloadmanager.h"
#include "filehash.h"
#include "imagecache.h"
#include "losnimport.h"
#include "metaparser.h"
#include "prefsdlg.h"
#include "progressbar.h"
#include "queryor.h"
#include "remotesource.h"
#include "scanner.h"
#include "settingsexport.h"
#include "stray.h"
#include "straydialog.h"
#include "strayresolve.h"
#include "tabbar.h"
#include "faviconcache.h"
#include "sidecarrefresh.h"
#include "tagsuggest.h"
#include "theme.h"
#include "uicons.h"
#include "scanner.h"
#include "tagtypes.h"
#include "theme.h"
#include "thumbloader.h"
#include "viewerpage.h"
#include "welcomepage.h"
#include <QAbstractItemView>
#include <QApplication>
#include <QActionGroup>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QCompleter>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QDrag>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QWidgetAction>
#include <QMenuBar>
#include <QMessageBox>
#include <QProcess>
#include <QMimeData>
#include <QPainter>
#include <QPushButton>
#include <QProgressBar>
#include <QProgressDialog>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QShortcut>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStringListModel>
#include <QStyledItemDelegate>
#include <QTemporaryFile>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QtConcurrent>
#include <climits>
#include <functional>
#include <utility>
#include <algorithm>

void MainWindow::refreshViewerTags(const BrowserTab *t) {
    if (!t || !m_viewer || m_pages->currentIndex() != 1)
        return;
    // The viewer owns a snapshot (m_current) taken when the post opened.
    // Classification verdicts land later and only touched browseMerged, so
    // the detail panel kept showing those tags in General - the "some
    // posts categorise, some don't" report (it depended on whether the
    // batch beat the click).
    for (Post &cur : m_current) {
        if (cur.isRemote) {
            const QVector<Post> &fresh =
                t->isFavourites ? t->localPosts : t->browseMerged;
            for (const Post &p : fresh) {
                if (p.isRemote && p.id == cur.id && p.siteKey == cur.siteKey) {
                    cur = p;
                    break;
                }
            }
        }
    }
    m_viewer->setPosts(m_current);
    m_viewer->refreshCurrentPost();
}

QHash<QString, QString> MainWindow::hydrusNamespaceBuckets() {
    const QString path = m_hydrusDb.isOpen()
                             ? m_hydrusDb.path()
                             : effectiveHydrusDbPath(m_settings);
    if (path.isEmpty())
        return {};
    const QFileInfo info(path);
    if (!info.exists())
        return {};
    // While the Hydrus tab holds the DB open, WAL writes may not touch the
    // main file, so mix the WAL size/mtime in. A temporary read-only open
    // (below) can itself create the WAL, so do not use it in that case.
    qint64 stamp = info.size() ^ info.lastModified().toMSecsSinceEpoch();
    if (m_hydrusDb.isOpen() && m_hydrusDb.path() == path) {
        const QFileInfo wal(path + QStringLiteral("-wal"));
        if (wal.exists())
            stamp ^= (wal.size() * 1315423911LL) ^
                     wal.lastModified().toMSecsSinceEpoch();
    }
    if (m_hydrusBucketsStamp >= 0 && m_hydrusBucketsPath == path &&
        m_hydrusBucketsStamp == stamp)
        return m_hydrusBuckets;
    QHash<QString, QString> buckets;
    if (m_hydrusDb.isOpen() && m_hydrusDb.path() == path) {
        buckets = m_hydrusDb.namespaceBuckets();
    } else {
        // Read-only, local, no settings side effects: do not create the
        // DB just because the classifier wants a dictionary.
        HydrusStore::Db db;
        if (db.open(path))
            buckets = db.namespaceBuckets();
    }
    m_hydrusBucketsPath = path;
    m_hydrusBucketsStamp = stamp;
    m_hydrusBuckets = buckets;
    return buckets;
}

QHash<QString, QString> MainWindow::hydrusBucketsFor(
    const QSet<QString> &tags) {
    QHash<QString, QString> out;
    if (tags.isEmpty())
        return out;
    const QHash<QString, QString> all = hydrusNamespaceBuckets();
    if (all.isEmpty())
        return out;
    for (const QString &tag : tags) {
        const auto it = all.constFind(tag);
        if (it != all.constEnd())
            out.insert(tag, it.value());
    }
    return out;
}

void MainWindow::applyClassifySettings() {
    const QString backend = classifyBackendKey(m_settings.classifyBackend);
    if (m_fetcher)
        m_fetcher->setBackend(backend, m_settings.classifyCustomUrl,
                              m_settings.classifyCustomFormat);
    if (m_browseFetcher)
        m_browseFetcher->setBackend(backend, m_settings.classifyCustomUrl,
                                    m_settings.classifyCustomFormat);
}

void MainWindow::maybeClassifyBrowse(BrowserTab *t) {
    // Same cached pass for remote grids and Favourites: both carry
    // category-less tags (LOSN rows, flat API blobs) that live in
    // `untyped` until a verdict exists. Manual overrides apply even when
    // automatic classification is off (they need no network).
    if (!t || (!t->isRemote && !t->isFavourites) || t->isHydrus)
        return;
    const bool autoOn = classifyAutoEnabled(m_settings);
    const QString backend = classifyBackendKey(m_settings.classifyBackend);
    const bool online = autoOn && classifyBackendIsOnline(backend);
    if (!autoOn && m_settings.manualTagTypes.isEmpty())
        return;
    QVector<Post> &target = t->isFavourites ? t->localPosts : t->browseMerged;
    // Manual overrides are explicit user intent: they apply to every post,
    // opted-out entries included. Fetched/cached verdicts do not.
    QHash<QString, QString> knownManual;
    for (auto it = m_settings.manualTagTypes.begin();
         it != m_settings.manualTagTypes.end(); ++it)
        knownManual.insert(it.key(), it.value());
    int manualMoved = 0;
    if (!knownManual.isEmpty()) {
        manualMoved =
            static_cast<int>(applyTagBuckets(target, knownManual).size());
        if (manualMoved > 0 && t->isFavourites)
            persistFavBuckets(target);
    }
    // Entries that opt out keep their tags as they arrive: collect and
    // apply verdicts for the rest only, preserving post order.
    QVector<Post> work;
    QVector<int> workIndex;
    for (int i = 0; i < target.size(); ++i) {
        if (!RemotePosts::skipAutoClassify(target.at(i))) {
            work.append(target.at(i));
            workIndex.append(i);
        }
    }
    QSet<QString> unknown;
    if (autoOn) {
        for (const Post &p : work) {
            for (const QString &tag :
                 p.untyped.split(QChar::Space, Qt::SkipEmptyParts))
                unknown.insert(tag.toLower());
        }
        for (auto it = knownManual.begin(); it != knownManual.end(); ++it)
            unknown.remove(it.key());
    }
    if (unknown.isEmpty() && manualMoved == 0)
        return;
    const QString db = effectiveTagTypesDbPath(m_settings); // one store for everything
    QHash<QString, QString> known;
    QStringList missing;
    // Hydrus-local backend: namespaced tags already in hydrus.db are a
    // free, offline dictionary. It wins over stale fetched verdicts.
    if (autoOn && backend == QStringLiteral("hydrus")) {
        const QHash<QString, QString> local = hydrusBucketsFor(unknown);
        for (auto it = local.begin(); it != local.end(); ++it) {
            if (knownManual.contains(it.key()))
                continue;
            known.insert(it.key(), it.value());
            unknown.remove(it.key());
        }
    } else if (online && !unknown.isEmpty()) {
        TagTypeCache cache;
        if (cache.open(db)) {
            for (const QString &tag : unknown) {
                int ty = TagTypes::kTransientFailure;
                if (cache.get(tag, ty)) {
                    if (ty != TagTypes::kUnknownTag)
                        known.insert(tag, TagTypes::bucketForType(ty));
                    // negatives stay general without refetching
                } else {
                    missing.append(tag);
                }
            }
            cache.close();
        } else {
            missing = QStringList(unknown.begin(), unknown.end());
        }
    }
    // Manual overrides win over fetched verdicts (and need no fetch).
    for (auto it = knownManual.begin(); it != knownManual.end(); ++it)
        known.insert(it.key(), it.value());
    if (!known.isEmpty() || manualMoved > 0) {
        const int moved = manualMoved + applyHostBuckets(work, known);
        for (int j = 0; j < work.size(); ++j)
            target[workIndex.at(j)] = work.at(j);
        if (moved > 0 && t->isFavourites)
            persistFavBuckets(target);
        if (t == currentTab()) {
            if (t->isFavourites) {
                applyFilter(); // favourites filter straight from localPosts
            } else {
                m_browseMerged = t->browseMerged;
                applyBrowseFilter();
                rebuildBrowseSidebar();
                rebuildCombos();
                updateResultLabel();
            }
            if (moved > 0)
                statusBar()->showMessage(
                    backend == QStringLiteral("hydrus")
                        ? tr("Categorized %1 tags from Hydrus namespaces")
                              .arg(moved)
                        : tr("Categorized %1 tags from cache").arg(moved),
                    8000);
            refreshViewerTags(t);
        }
    }
    if (!missing.isEmpty() && online) {
        missing.sort();
        m_classifyTab = t;
        m_browseFetcher->fetch(missing, db);
        if (t == currentTab())
            statusBar()->showMessage(
                tr("Classifying %1 untyped tags…").arg(missing.size()));
    }
}

void MainWindow::onBrowseClassifyFinished(
    const QHash<QString, QString> &buckets) {
    m_scanBar->setVisible(false);
    BrowserTab *t = m_classifyTab;
    m_classifyTab = nullptr;
    m_pendingBrowseBuckets.clear(); // full map below supersedes partials
    if (!t || !m_tabs.contains(t))
        return; // tab closed mid-fetch
    const int failed = m_browseFetcher->failures();
    const QString backendError = m_browseFetcher->backendError();
    if (buckets.isEmpty()) {
        if (t == currentTab()) {
            if (!backendError.isEmpty())
                statusBar()->showMessage(backendError, 12000);
            else
                statusBar()->showMessage(
                    failed > 0
                        ? tr("Tag classification: %1 failed (network), "
                             "will retry next scan")
                              .arg(failed)
                        : tr("Tag classification done"),
                    8000);
        }
        return;
    }
    // Fetched verdicts skip opted-out entries (manual overrides were
    // already applied up front and are unaffected).
    QVector<Post> &target = t->isFavourites ? t->localPosts : t->browseMerged;
    QVector<Post> work;
    QVector<int> workIndex;
    for (int i = 0; i < target.size(); ++i) {
        if (!RemotePosts::skipAutoClassify(target.at(i))) {
            work.append(target.at(i));
            workIndex.append(i);
        }
    }
    int moved = applyHostBuckets(work, buckets);
    for (int j = 0; j < work.size(); ++j)
        target[workIndex.at(j)] = work.at(j);
    if (moved > 0 && t->isFavourites)
        persistFavBuckets(target);
    if (t != currentTab())
        return; // stashed; shows classified when activated
    if (t->isFavourites) {
        applyFilter();
    } else {
        m_browseMerged = t->browseMerged;
        applyBrowseFilter();
        rebuildBrowseSidebar();
        rebuildCombos();
        updateResultLabel();
    }
    refreshViewerTags(t);
    QString msg = tr("Categorized %1 tags (%2 resolved)")
                      .arg(moved)
                      .arg(buckets.size());
    if (failed > 0)
        msg += tr(" · %1 failed, will retry").arg(failed);
    if (!backendError.isEmpty())
        msg += QStringLiteral(" · ") + backendError;
    statusBar()->showMessage(msg, 12000);
}

bool MainWindow::classifyUiTick(int done, int total) {
    // 1500 status-bar relayouts + bar paints is its own freeze: refresh at
    // most every 200ms (and always on completion).
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (done < total && now - m_lastClassifyUi < 200)
        return false;
    m_lastClassifyUi = now;
    return true;
}

void MainWindow::onClassifyProgress(int done, int total) {
    if (!classifyUiTick(done, total))
        return;
    m_scanBar->setVisible(true);
    m_scanBar->setRange(0, qMax(1, total));
    m_scanBar->setValue(done);
    statusBar()->showMessage(
        tr("Classifying tags… %1 / %2").arg(done).arg(total));
}

void MainWindow::onClassifyPartial(const QHash<QString, QString> &buckets) {
    if (m_manualActive || buckets.isEmpty())
        return; // manual popup run owns its own UI
    for (auto it = buckets.begin(); it != buckets.end(); ++it)
        m_pendingBuckets.insert(it.key(), it.value());
    if (!m_partialTimer->isActive())
        m_partialTimer->start();
}

void MainWindow::onBrowseClassifyPartial(
    const QHash<QString, QString> &buckets) {
    if (buckets.isEmpty())
        return;
    for (auto it = buckets.begin(); it != buckets.end(); ++it)
        m_pendingBrowseBuckets.insert(it.key(), it.value());
    if (!m_partialTimer->isActive())
        m_partialTimer->start();
}

void MainWindow::flushClassifyPartials() {
    if (!m_pendingBuckets.isEmpty()) {
        const QHash<QString, QString> batch = std::move(m_pendingBuckets);
        m_pendingBuckets.clear();
        if (m_index.applyTagTypes(batch) > 0 && !isBrowse())
            rebuildSidebar();
    }
    if (!m_pendingBrowseBuckets.isEmpty()) {
        const QHash<QString, QString> batch = std::move(m_pendingBrowseBuckets);
        m_pendingBrowseBuckets.clear();
        BrowserTab *t = m_classifyTab;
        if (!t || !m_tabs.contains(t))
            return; // tab closed mid-fetch; finished() drops it too
        QVector<Post> &target =
            t->isFavourites ? t->localPosts : t->browseMerged;
        QVector<Post> work;
        QVector<int> workIndex;
        for (int i = 0; i < target.size(); ++i) {
            if (!RemotePosts::skipAutoClassify(target.at(i))) {
                work.append(target.at(i));
                workIndex.append(i);
            }
        }
        if (!work.isEmpty()) {
            const int moved = applyHostBuckets(work, batch);
            for (int j = 0; j < work.size(); ++j)
                target[workIndex.at(j)] = work.at(j);
            if (moved > 0 && t->isFavourites)
                persistFavBuckets(target);
            if (t == currentTab()) {
                if (t->isFavourites) {
                    applyFilter();
                } else {
                    m_browseMerged = t->browseMerged;
                    rebuildBrowseSidebar();
                    refreshViewerTags(t);
                }
            }
        }
    }
}

void MainWindow::onClassifyFinished(const QHash<QString, QString> &buckets) {
    if (m_manualActive)
        return; // manual popup run: onManualClassifyFinished owns it
    m_pendingBuckets.clear(); // everything below re-applies the full map
    m_scanBar->setVisible(false);
    const int failed = m_fetcher->failures();
    const QString backendError = m_fetcher->backendError();
    if (buckets.isEmpty()) {
        if (!backendError.isEmpty())
            statusBar()->showMessage(backendError, 12000);
        else
            statusBar()->showMessage(
                failed > 0
                    ? tr("Tag classification: %1 failed (network), "
                         "will retry next scan")
                          .arg(failed)
                    : tr("Tag classification done"),
                8000);
        return;
    }
    const int moved = m_index.applyTagTypes(buckets);
    rebuildCombos();
    rebuildSidebar();
    applyFilter();
    QString msg = tr("Categorized %1 tags (%2 resolved)")
                      .arg(moved)
                      .arg(buckets.size());
    if (failed > 0)
        msg += tr(" · %1 failed, will retry").arg(failed);
    if (!backendError.isEmpty())
        msg += QStringLiteral(" · ") + backendError;
    statusBar()->showMessage(msg, 12000);
}

