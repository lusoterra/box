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

void MainWindow::prefetchFavicons() {
    if (!m_favicons)
        return;
    for (const BooruSite &b : m_settings.boorus)
        m_favicons->prefetch(b.key, b.baseUrl);
}

void MainWindow::refreshFavicons() {
    if (!m_favicons)
        return;
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
    connect(dlAct, &QAction::triggered, this,
            [this] { addLocalTab(effectiveDownloadDir(m_settings)); });
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
    updateTabTitle(m_curTab);
    doBrowseSearch(false);
}

// Infinite scroll: fetch the next page and append (throttled against 429s).
void MainWindow::onViewerPastEnd() {
    if (!isBrowse() || m_loadingPage)
        return;
    // Fetch more, then step into the first new post.
    m_advanceRow = m_current.size();
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
    updateTabTitle(m_tabs.size() - 1);
    m_tabBar->setCurrentIndex(m_tabs.size());
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
    int browseTotal = 0;
    if (isBrowse()) {
        for (const Post &p : m_browseMerged) {
            browseRatings[p.rating]++;
            browseTotal++;
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
        const int n = isBrowse() ? (key == QStringLiteral("all") ? browseTotal
                                                                 : browseRatings.value(key))
                                 : (key == QStringLiteral("all") ? s.total
                                                                 : s.ratings.value(key));
        m_ratingBox->addItem(QStringLiteral("%1 (%2)").arg(label).arg(n), key);
    }
    const int ri = m_ratingBox->findData(m_rating);
    m_ratingBox->setCurrentIndex(qMax(0, ri));
    m_ratingBox->blockSignals(false);

    m_categoryBox->blockSignals(true);
    m_categoryBox->clear();
    m_categoryBox->addItem(tr("All sources (%1)").arg(s.total), QString());
    QVector<QPair<QString, int>> cats;
    cats.reserve(s.categories.size());
    for (auto it = s.categories.begin(); it != s.categories.end(); ++it)
        cats.append({it.key(), it.value()});
    std::sort(cats.begin(), cats.end(),
              [](const auto &a, const auto &b) { return a.second > b.second; });
    for (const auto &[name, n] : cats)
        m_categoryBox->addItem(QStringLiteral("%1 (%2)").arg(name).arg(n), name);
    const int ci = m_categoryBox->findData(m_category);
    m_categoryBox->setCurrentIndex(qMax(0, ci));
    m_categoryBox->blockSignals(false);
}

