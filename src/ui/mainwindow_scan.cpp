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

void MainWindow::openPath(const QString &path) {
    if (path.isEmpty() || !QDir(path).exists())
        return;
    addLocalTab(path);
}

void MainWindow::showWelcome() {
        // Prune dead entries so the list never rots.
    QStringList kept;
    for (const QString &p : m_settings.history) {
        if (QDir(p).exists() && !kept.contains(p))
            kept.append(p);
    }
    if (kept != m_settings.history) {
        m_settings.history = kept.mid(0, 10);
        saveSettings(m_settings);
    }
    m_welcome->setRestore(m_settings.tabsDefault == QStringLiteral("last"));
    m_welcome->setLastPlace(describeLastPlace());
    m_curTab = -1;
    if (m_viewer)
        m_viewer->stopPlayback(); // home hides the viewer: same audio leak
    m_tabBar->blockSignals(true);
    m_tabBar->setCurrentIndex(0);
    m_tabBar->blockSignals(false);
    m_stage->setCurrentIndex(0);
    setWindowTitle(QStringLiteral("Box"));
}

void MainWindow::openFolder() {
    const QString dir = QFileDialog::getExistingDirectory(
        this, tr("Open booru folder"), m_root);
    if (dir.isEmpty())
        return;
    m_settings.lastRoot = dir;
    noteFolderOpened(m_settings, dir);
    saveSettings(m_settings);
    addLocalTab(dir);
}

void MainWindow::reload() {
    if (isBrowse()) {
        doBrowseSearch();
        return;
    }
    if (BrowserTab *t = currentTab(); t && t->isFavourites) {
        refreshFavTabs(); // re-list the DB, not the archive on disk
        return;
    }
    if (m_curTab >= 0)
        startScan(m_root);
}

void MainWindow::loadStylesheet() {
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Load custom Qt stylesheet"), QDir::homePath(),
        tr("Stylesheets (*.qss);;All files (*)"));
    if (path.isEmpty())
        return;
    if (!Theme::applyStylesheetFile(*qApp, path))
        QMessageBox::warning(this, tr("Stylesheet"),
                             tr("Could not read %1").arg(path));
}

void MainWindow::cancelScan() {
    // Stop a running archive scan so rescans and quit never pile up or hang.
    if (m_scanner)
        m_scanner->requestCancel();
    if (m_scanWatcher) {
        m_scanWatcher->cancel();
        m_scanWatcher->deleteLater();
        m_scanWatcher = nullptr;
    }
    if (m_scanner) {
        m_scanner->deleteLater();
        m_scanner = nullptr;
    }
    m_scanBar->setVisible(false);
}

void MainWindow::startScan(const QString &root) {
    m_fetcher->cancel();
    cancelScan(); // never stack giant scans (quit also relies on this)
    m_root = root;
    m_stage->setCurrentIndex(1);
    setWindowTitle(QStringLiteral("Box - %1").arg(root));
    statusBar()->showMessage(tr("Scanning %1 …").arg(root));
    auto *scanner = new Scanner;
    auto *watcher = new ScanWatcher;
    m_scanner = scanner;
    m_scanWatcher = watcher;
    connect(scanner, &Scanner::progress, this, &MainWindow::onScanProgress);
    connect(scanner, &Scanner::cacheStats, this, &MainWindow::onCacheStats);
    connect(watcher, &QFutureWatcher<QVector<Post>>::finished, this,
            [this, watcher, scanner] {
                if (watcher != m_scanWatcher) {
                    // Superseded by a newer scan (or quit): drop the result.
                    watcher->deleteLater();
                    if (scanner != m_scanner)
                        scanner->deleteLater();
                    return;
                }
                m_scanWatcher = nullptr;
                m_scanner = nullptr;
                QVector<Post> scanned = watcher->result();
                healLocalPosts(scanned);
                m_index.setPosts(scanned);
                watcher->deleteLater();
                scanner->deleteLater();
                onScanFinished();
            });
    watcher->setFuture(QtConcurrent::run(
        &Scanner::scan, root, scanner, m_settings.indexCache,
        static_cast<QSet<QString> *>(nullptr)));
}

void MainWindow::onScanProgress(int n, int total) {
    m_scanBar->setVisible(true);
    m_scanBar->setRange(0, qMax(1, total));
    m_scanBar->setValue(n);
    statusBar()->showMessage(tr("Scanning… %1 / %2 files").arg(n).arg(total));
}

void MainWindow::onCacheStats(int hits, int misses) {
    statusBar()->showMessage(
        tr("Index cache: %1 reused, %2 parsed").arg(hits).arg(misses), 8000);
}

void MainWindow::onScanFinished() {
    const Stats s = m_index.stats();
    m_countLabel->setText(tr("%1 posts").arg(s.total));
    updateDots();
    m_scanBar->setVisible(false);
    statusBar()->showMessage(tr("Found %1 posts").arg(s.total), 5000);
    // Local archives are never modified: no history, no last-root, no
    // session writes (tabs are for browsing; the library stays untouched).
    if (BrowserTab *t = currentTab(); t && !t->isRemote && t->root == m_root)
        t->localPosts = m_index.posts();
    rebuildCombos();
    rebuildSidebar();
    applyFilter();
    maybeClassify();
}

bool MainWindow::maybeClassify(bool force) {
    const bool online = m_settings.classifyTags;
    const QSet<QString> unknown =
        (online || force) ? m_index.collectUntyped() : QSet<QString>();
    if (unknown.isEmpty() && m_settings.manualTagTypes.isEmpty())
        return false;
    const QString db = effectiveTagTypesDbPath(m_settings);
    TagTypeCache cache;
    QHash<QString, QString> known;
    QStringList missing;
    if (!unknown.isEmpty() && !force && cache.open(db)) {
        for (const QString &t : unknown) {
            int ty = TagTypes::kTransientFailure;
            if (cache.get(t, ty)) {
                if (ty != TagTypes::kUnknownTag)
                    known.insert(t, TagTypes::bucketForType(ty));
                // negatives stay general without refetching
            } else {
                missing.append(t);
            }
        }
        cache.close();
    } else if (!unknown.isEmpty()) {
        if (force)
            statusBar()->showMessage(
                tr("Refreshing categories for %1 tags…").arg(unknown.size()));
        missing = QStringList(unknown.begin(), unknown.end());
    }
    // Manual overrides: no network, win over fetched verdicts, and keep
    // pinned tags (even General) out of the fetch queue.
    for (auto it = m_settings.manualTagTypes.begin();
         it != m_settings.manualTagTypes.end(); ++it) {
        known.insert(it.key(), it.value());
        missing.removeAll(it.key());
    }
    if (!known.isEmpty()) {
        const int moved = m_index.applyTagTypes(known);
        if (moved > 0) {
            rebuildCombos();
            rebuildSidebar();
            applyFilter();
            statusBar()->showMessage(
                tr("Categorized %1 tags from cache").arg(moved), 8000);
        }
    }
    if (!missing.isEmpty() && (online || force)) {
        missing.sort();
        statusBar()->showMessage(
            tr("Classifying %1 untyped tags…").arg(missing.size()));
        m_fetcher->fetch(missing, db);
        return true;
    }
    return false;
}

int MainWindow::applyHostBuckets(QVector<Post> &work,
                                 const QHash<QString, QString> &buckets) {
    if (buckets.isEmpty() || work.isEmpty())
        return 0;
    // Group by entry so each group's verdicts fold through its own
    // taxonomy (multibooru: a meme host and danbooru classify together).
    QHash<QString, QVector<int>> groups;
    for (int i = 0; i < work.size(); ++i) {
        const Post &p = work.at(i);
        const QString key = !p.siteKey.isEmpty()
                                ? p.siteKey
                                : RemotePosts::keyForLabel(p.siteName);
        groups[key].append(i);
    }
    int moved = 0;
    for (auto it = groups.begin(); it != groups.end(); ++it) {
        QHash<QString, QString> folded = buckets;
        const Post &rep = work.at(it.value().first());
        for (auto b = folded.begin(); b != folded.end(); ++b)
            b.value() = RemotePosts::foldedBucketFor(rep, b.value());
        QVector<Post> sub;
        sub.reserve(it.value().size());
        for (int i : it.value())
            sub.append(work.at(i));
        moved += applyTagBuckets(sub, folded).size();
        for (int j = 0; j < it.value().size(); ++j)
            work[it.value().at(j)] = sub.at(j);
    }
    return moved;
}

