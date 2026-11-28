#include "mainwindow.h"

#include "cookiegrab.h"
#include "netpolicy.h"
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

void MainWindow::ensureWebBackend() {
    if (m_webDownloads)
        return;
    // First WebEngine object in the process. Built only after the user has
    // explicitly browsed or downloaded, so a fresh profile stays offline.
    m_webDownloads = new WebEngineDownloadBackend(this);
    m_downloads->setWebBackend(m_webDownloads);
    m_thumbs->setWebBackend(m_webDownloads);
    if (m_viewer)
        m_viewer->setWebBackend(m_webDownloads);
    // Warm up Chromium at idle now that it is allowed: the first visible
    // WebEngine use otherwise stalls the GUI thread. Keep the delay so the
    // browse action itself paints first.
    QTimer::singleShot(20000, this, [this] {
        CookieGrabDialog::prime(effectiveDataDir(m_settings) +
                                QStringLiteral("/webprofile"));
    });
}

void MainWindow::allowNetwork() {
    // Just open the gate. Callers that actually need the browser stack or
    // site icons build/fetch them explicitly, so a favourites tab does not
    // pay for Chromium or a favicon round-trip.
    NetworkPolicy::allow();
}

void MainWindow::prefetchFavicons() {
    if (!m_favicons)
        return;
    for (const BooruSite &b : m_settings.boorus)
        m_favicons->prefetch(b.key, b.baseUrl);
}

void MainWindow::refreshFavicons() {
    if (!m_favicons)
        return;
    NetworkPolicy::allow(); // explicit user action: fetching is expected
    m_favicons->refreshAll();
    prefetchFavicons();
    rebuildSitesMenu();
    for (int i = 0; i < m_tabs.size(); ++i)
        updateTabTitle(i);
    statusBar()->showMessage(tr("Refreshing favicons…"), 4000);
}

void MainWindow::showPlusMenu() {
    QMenu menu(this);
    QAction *homeAct = menu.addAction(
        UiIcons::icon(QStringLiteral("house")), tr("Home"));
    connect(homeAct, &QAction::triggered, this, [this] { showWelcome(); });
    QAction *favAct = menu.addAction(
        UiIcons::heart(UiIcons::favRed(), true),
        tr("Favourites"));
    connect(favAct, &QAction::triggered, this, &MainWindow::openFavTab);
    QAction *dlAct = menu.addAction(UiIcons::icon(QStringLiteral("download")),
                                    tr("Downloads"));
    // Downloads is an online-booru view (the downloaded media of the
    // configured sites), never a local archive tab.
    connect(dlAct, &QAction::triggered, this,
            [this] { addRemoteTab({kSrcDownloads}, {}); });
    if (m_settings.hydrusTab) {
        QAction *hydrusAct = menu.addAction(
            UiIcons::icon(QStringLiteral("hydrus")), tr("Hydrus"));
        hydrusAct->setToolTip(
            tr("Import and browse media from the local Hydrus library"));
        connect(hydrusAct, &QAction::triggered, this,
                &MainWindow::openHydrusTab);
    }
    // Saved local archives: one click each, exactly like a booru tab.
    for (const AppSettings::SavedArchive &a : m_settings.archives) {
        if (a.root.trimmed().isEmpty() || !QDir(a.root).exists())
            continue;
        QAction *archiveAct = menu.addAction(
            UiIcons::icon(QStringLiteral("archive")),
            a.name.trimmed().isEmpty() ? QDir(a.root).dirName()
                                      : a.name.trimmed());
        archiveAct->setToolTip(QDir::toNativeSeparators(a.root));
        connect(archiveAct, &QAction::triggered, this,
                [this, root = a.root] { addLocalTab(root); });
    }
    menu.addSeparator();
    // The read-only archive browser is easy to forget: always offer it,
    // alongside the optional plain "open local folder" entry.
    QAction *viewAct = menu.addAction(
        UiIcons::icon(QStringLiteral("folder-open")),
        tr("Open view-only folder…"));
    viewAct->setToolTip(
        tr("Browse a gallery-dl/LOSN archive without changing it"));
    connect(viewAct, &QAction::triggered, this, &MainWindow::openFolder);
    if (m_settings.showLocalFolderInPlusMenu) {
        QAction *openAct = menu.addAction(
            UiIcons::icon(QStringLiteral("folder-open")),
            tr("Open local folder…"));
        connect(openAct, &QAction::triggered, this, &MainWindow::openFolder);
    }
    menu.addSeparator();
    // One click = one tab. Merging stays available per-tab through the
    // "Boorus…" menu, which checks any set of sources into the open tab.
    for (const BooruSite &s : m_settings.boorus) {
        if (!s.enabled)
            continue;
        QAction *a = menu.addAction(m_favicons->iconFor(s.key, s.baseUrl),
                                    s.label);
        a->setData(s.key);
        connect(a, &QAction::triggered, this, [this, a] {
            addRemoteTab({a->data().toString()}, {});
        });
    }
    menu.exec(m_plusBtn->mapToGlobal(QPoint(0, m_plusBtn->height())));
}

void MainWindow::syncSitesMenu() {
    BrowserTab *t = currentTab();
    const QStringList active = t ? t->sites : m_settings.browseSites;
    for (QAction *a : m_sitesMenu->actions())
        a->setChecked(active.contains(a->data().toString()));
}

void MainWindow::onSitesChanged() {
    BrowserTab *t = currentTab();
    if (!t || !t->isRemote)
        return;
    QStringList sites;
    for (QAction *a : m_sitesMenu->actions()) {
        if (a->isChecked())
            sites << a->data().toString();
    }
    if (sites.isEmpty()) {
        syncSitesMenu(); // refuse to uncheck everything
        return;
    }
    updateSortBox();
    t->sites = sites;
    t->page = 1;
    // The Downloads-only media picker follows the source list.
    setBrowseUi(true);
    syncMediaBox();
    updateTabTitle(m_curTab);
    doBrowseSearch(false);
}

// Infinite scroll: fetch the next page and append (throttled against 429s).
void MainWindow::onViewerPastEnd() {
    if (!isBrowse() || m_loadingPage)
        return;
    // Fetch more, then step into the first new post.
    m_advanceRow = static_cast<int>(m_current.size());
    m_advanceOnLoad = true;
    loadNextPage();
    if (!m_loadingPage) {
        // Nothing to load (cooldown); drop the request.
        m_advanceOnLoad = false;
    }
}

void MainWindow::setAsMascot(const Post &post) {
    if (post.absFile.isEmpty() || !QFileInfo::exists(post.absFile)) {
        statusBar()->showMessage(tr("No local file to use as mascot"), 4000);
        return;
    }
    if (!WelcomePage::isMascotFile(post.absFile)) {
        statusBar()->showMessage(
            tr("Only pictures can be used as the home mascot (no videos)"),
            5000);
        return;
    }
    m_settings.mascotPath = post.absFile;
    m_settings.mascotHidden = false;
    saveSettings(m_settings);
    m_welcome->setMascot(effectiveMascotPath(m_settings));
    statusBar()->showMessage(tr("Home mascot updated"), 4000);
}

void MainWindow::openFavTab() {
    // Every call opens a fresh tab (like remote tabs): use one per tag set,
    // e.g. different filters in different tabs. Shared state stays in the
    // DB; per-tab filters live in the tab struct.
    auto *t = new BrowserTab();
    t->isFavourites = true;
    t->sort = QStringLiteral("added"); // DB order is newest-first
    t->title = tr("Favourites");
    m_tabs.append(t);
    m_tabBar->addTab(QString());
    updateTabTitle(static_cast<int>(m_tabs.size()) - 1);
    m_tabBar->setCurrentIndex(static_cast<int>(m_tabs.size()));
    saveTabsNow();
}

// ── Tag refresh ──────────────────────────────────────────────

QString MainWindow::refreshReason(const Post &post) {
    // Library archives are read-only: no sidecar writes, no edits.
    if (!post.isRemote) {
        const QString dlRoot =
            QDir(effectiveDownloadDir(m_settings)).canonicalPath();
        const QString absDir =
            QDir(QFileInfo(post.absFile).absolutePath()).canonicalPath();
        if (dlRoot.isEmpty() || absDir.isEmpty() ||
            (absDir != dlRoot && !absDir.startsWith(dlRoot + u'/')))
            return tr("Library archives are read-only");
    }
    RefreshTarget t = resolveRefreshTarget(post);
    if (!t.valid()) {
        if (!post.isRemote)
            return tr("No source link (gallery-dl dumps can't refresh)");
        QString key = post.siteKey; // stamped rows name their entry
        if (key.isEmpty() || !RemoteSource::siteFor(key))
            key = RemotePosts::keyForLabel(post.siteName);
        const BooruSite *site = key.isEmpty() ? nullptr : RemoteSource::siteFor(key);
        if (site && site->baseUrl.isEmpty())
            return tr("Set the %1 instance URL first").arg(site->label);
        return tr("Nothing refreshable");
    }
    if (t.siteKey == QStringLiteral("gelbooru") && !credsFor(t.siteKey).complete())
        return tr("Needs Gelbooru API key (Preferences → Booru manager)");
    return {};
}

void MainWindow::rebuildCombos() {
    const Stats s = m_index.stats();
    // Browse tabs filter the merged results: counts come from them, not
    // from whatever local archive was scanned last.
    QHash<QString, int> browseRatings;
    if (isBrowse()) {
        for (const Post &p : m_browseMerged) {
            browseRatings[p.rating]++;
        }
    }
    m_ratingBox->blockSignals(true);
    m_ratingBox->clear();
    const QList<QPair<QString, QString>> ratings = {
        {QStringLiteral("all"), tr("All")},
        {QStringLiteral("general"), tr("General")},
        {QStringLiteral("sensitive"), tr("Sensitive")},
        {QStringLiteral("questionable"), tr("Questionable")},
        {QStringLiteral("explicit"), tr("Explicit")},
    };
    for (const auto &[key, label] : ratings) {
        m_ratingBox->addItem(label, key);
    }
    const int ri = m_ratingBox->findData(m_rating);
    m_ratingBox->setCurrentIndex(qMax(0, ri));
    m_ratingBox->blockSignals(false);

    // Source list follows the tab's own rows: favourites mix boorus and
    // folders, a downloads view mixes boorus/archive folders. Plain
    // browse tabs hide this combo (one source, picker above).
    QHash<QString, int> sourceCounts;
    int sourceTotal = 0;
    if (const BrowserTab *t = currentTab(); t && t->isFavourites) {
        for (const Post &p : t->localPosts) {
            if (!p.category.isEmpty())
                sourceCounts[p.category]++;
            ++sourceTotal;
        }
    } else if (isBrowse()) {
        for (const Post &p : m_browseMerged) {
            if (!p.category.isEmpty())
                sourceCounts[p.category]++;
            ++sourceTotal;
        }
    } else {
        sourceCounts = s.categories;
        sourceTotal = s.total;
    }
    m_categoryBox->blockSignals(true);
    m_categoryBox->clear();
    m_categoryBox->addItem(tr("All sources (%1)").arg(sourceTotal), QString());
    QVector<QPair<QString, int>> cats;
    cats.reserve(sourceCounts.size());
    for (auto it = sourceCounts.begin(); it != sourceCounts.end(); ++it)
        cats.append({it.key(), it.value()});
    std::sort(cats.begin(), cats.end(),
              [](const auto &a, const auto &b) { return a.second > b.second; });
    for (const auto &[name, n] : cats)
        m_categoryBox->addItem(QStringLiteral("%1 (%2)").arg(name).arg(n), name);
    const int ci = m_categoryBox->findData(m_category);
    m_categoryBox->setCurrentIndex(qMax(0, ci));
    m_categoryBox->blockSignals(false);
}

