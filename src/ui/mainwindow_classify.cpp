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

void MainWindow::maybeClassifyBrowse(BrowserTab *t) {    if (!t || t->isFavourites || !m_settings.classifyTags)
        return;
    // Manual overrides are explicit user intent: they apply to every post,
    // opted-out entries included. Fetched/cached verdicts do not.
    QHash<QString, QString> knownManual;
    for (auto it = m_settings.manualTagTypes.begin();
         it != m_settings.manualTagTypes.end(); ++it)
        knownManual.insert(it.key(), it.value());
    int manualMoved = 0;
    if (!knownManual.isEmpty())
        manualMoved = applyTagBuckets(t->browseMerged, knownManual).size();
    // Entries that opt out keep their tags as they arrive: collect and
    // apply verdicts for the rest only, preserving post order.
    QVector<Post> work;
    QVector<int> workIndex;
    for (int i = 0; i < t->browseMerged.size(); ++i) {
        if (!RemotePosts::skipAutoClassify(t->browseMerged.at(i))) {
            work.append(t->browseMerged.at(i));
            workIndex.append(i);
        }
    }
    QSet<QString> unknown;
    for (const Post &p : work) {
        for (const QString &tag :
             p.untyped.split(QChar::Space, Qt::SkipEmptyParts))
            unknown.insert(tag.toLower());
    }
    if (unknown.isEmpty() && manualMoved == 0)
        return;
    const QString db = effectiveTagTypesDbPath(m_settings); // one store for everything
    TagTypeCache cache;
    QHash<QString, QString> known;
    QStringList missing;
    if (!unknown.isEmpty() && cache.open(db)) {
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
    } else if (!unknown.isEmpty()) {
        missing = QStringList(unknown.begin(), unknown.end());
    }
    // Manual overrides win over fetched verdicts (and need no fetch).
    for (auto it = knownManual.begin(); it != knownManual.end(); ++it) {
        known.insert(it.key(), it.value());
        missing.removeAll(it.key());
    }
    if (!known.isEmpty() || manualMoved > 0) {
        const int moved =
            manualMoved + applyHostBuckets(work, known);
        for (int j = 0; j < work.size(); ++j)
            t->browseMerged[workIndex.at(j)] = work.at(j);
        if (t == currentTab()) {
            m_browseMerged = t->browseMerged;
            applyBrowseFilter();
            rebuildBrowseSidebar();
            rebuildCombos();
            updateResultLabel();
            if (moved > 0)
                statusBar()->showMessage(
                    tr("Categorized %1 tags from cache").arg(moved), 8000);
        }
    }
    if (!missing.isEmpty()) {
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
    if (buckets.isEmpty()) {
        if (t == currentTab())
            statusBar()->showMessage(
                failed > 0
                    ? tr("Tag classification: %1 failed (network), "
                         "will retry next scan")
                          .arg(failed)
                    : tr("Tag classification done"),
                8000);
        return;
    }
    // Fetched verdicts skip opted-out entries (manual overrides were
    // already applied up front and are unaffected).
    QVector<Post> work;
    QVector<int> workIndex;
    for (int i = 0; i < t->browseMerged.size(); ++i) {
        if (!RemotePosts::skipAutoClassify(t->browseMerged.at(i))) {
            work.append(t->browseMerged.at(i));
            workIndex.append(i);
        }
    }
    int moved = applyHostBuckets(work, buckets);
    for (int j = 0; j < work.size(); ++j)
        t->browseMerged[workIndex.at(j)] = work.at(j);
    if (t != currentTab())
        return; // stashed; shows classified when activated
    m_browseMerged = t->browseMerged;
    applyBrowseFilter();
    rebuildBrowseSidebar();
    rebuildCombos();
    updateResultLabel();
    QString msg = tr("Categorized %1 tags (%2 resolved)")
                      .arg(moved)
                      .arg(buckets.size());
    if (failed > 0)
        msg += tr(" · %1 failed, will retry").arg(failed);
    statusBar()->showMessage(msg, 8000);
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
        QVector<Post> work;
        QVector<int> workIndex;
        for (int i = 0; i < t->browseMerged.size(); ++i) {
            if (!RemotePosts::skipAutoClassify(t->browseMerged.at(i))) {
                work.append(t->browseMerged.at(i));
                workIndex.append(i);
            }
        }
        if (!work.isEmpty()) {
            applyHostBuckets(work, batch);
            for (int j = 0; j < work.size(); ++j)
                t->browseMerged[workIndex.at(j)] = work.at(j);
            if (t == currentTab()) {
                m_browseMerged = t->browseMerged;
                rebuildBrowseSidebar();
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
    if (buckets.isEmpty()) {
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
    statusBar()->showMessage(msg, 8000);
}

