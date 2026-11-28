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
#include "gridloadingoverlay.h"
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

void MainWindow::saveCurrentTab() {
    if (m_curTab < 0 || m_curTab >= m_tabs.size())
        return;
    BrowserTab *t = m_tabs[m_curTab];
    t->activeTags = m_activeTags;
    t->searchQuery = m_searchQuery;
    t->rating = m_rating;
    t->category = m_category;
    t->sort = m_sort;
    if (m_mediaBox && isDownloadsTab())
        t->mediaFilter = m_mediaBox->currentData().toString();
    int lo = 0, hi = -1;
    visibleRowRange(lo, hi);
    if (hi >= 0)
        t->scrollRow = lo; // keep the old position if the model was empty
    if (!t->isRemote) {
        if (!t->isFavourites && !t->isHydrus)
            t->localPosts = m_index.posts();
        // else: favourites list is DB-owned (re-listed on activation);
        // overwriting it here is what made new favourites vanish on tab
        // switches until the next manual refresh. Hydrus rows are also
        // DB-owned and must never borrow the archive index.
    } else
        t->browseMerged = m_browseMerged;
}

BrowserTab *MainWindow::currentTab() {
    if (m_curTab < 0 || m_curTab >= m_tabs.size())
        return nullptr;
    return m_tabs[m_curTab];
}

void MainWindow::saveTabs() {
    stageTabs();
    // Coalesce the ini write: switches and filter edits come in bursts
    // and nothing reads the file back mid-session. Structural changes
    // (add/close/move) bypass this via saveTabsNow(). Quit/restart
    // flush immediately.
    if (m_saveTimer && !m_saveTimer->isActive())
        m_saveTimer->start();
}

void MainWindow::saveTabsNow() {
    // Tab structure changed: persist synchronously so a crash inside
    // the 1500 ms coalesce window can't lose the new layout. saveSettings
    // is atomic (tmp+rename) with a .bak of the last-good file.
    if (m_saveTimer)
        m_saveTimer->stop();
    stageTabs();
    saveSettings(m_settings);
}

void MainWindow::flushSavedTabs() {
    if (m_saveTimer)
        m_saveTimer->stop();
    stageTabs();
    saveSettings(m_settings);
}

void MainWindow::stageTabs() {
    saveCurrentTab(); // flush omnibar/filters into the structs first
    // Last place follows the live tab (home itself never overwrites it).
    if (BrowserTab *c = currentTab()) {
        m_settings.hasLastPlace = true;
        AppSettings::SavedTab &lp = m_settings.lastPlace;
        lp.kind = c->isHydrus ? QStringLiteral("hydrus")
                              : (c->isFavourites ? QStringLiteral("favs")
                                                 : (c->isRemote
                                                        ? QStringLiteral("browse")
                                                        : QStringLiteral("local")));
        lp.root = c->isRemote ? QString() : c->root;
        lp.sites = c->isRemote ? c->sites : QStringList();
        lp.tags = c->activeTags;
        lp.query = c->searchQuery;
        lp.rating = c->rating;
        lp.category = c->category;
        lp.sort = c->sort;
        lp.hydrusOnlyMissing = c->hydrusOnlyMissing;
        lp.viewerKey = c->viewerKey;
        lp.scrollRow = c->scrollRow;
    }
    m_settings.savedTabs.clear();
    for (BrowserTab *t : m_tabs) {
        if (!t->isRemote && !t->isFavourites && !t->isHydrus)
            continue; // local archives never persist (library untouched)
        AppSettings::SavedTab st;
        if (t->isHydrus)
            st.kind = QStringLiteral("hydrus");
        else if (t->isFavourites)
            st.kind = QStringLiteral("favs");
        else if (t->isRemote) {
            st.kind = QStringLiteral("browse");
            st.sites = t->sites;
        } else {
            st.kind = QStringLiteral("local");
            st.root = t->root;
        }
        st.tags = t->activeTags;
        st.query = t->searchQuery;
        st.rating = t->rating;
        st.category = t->category;
        st.sort = t->sort;
        st.hydrusOnlyMissing = t->hydrusOnlyMissing;
        st.viewerKey = t->viewerKey;
        st.scrollRow = t->scrollRow;
        m_settings.savedTabs.append(st);
    }
    m_settings.savedTabCurrent = qMax(0, m_curTab);
}

bool MainWindow::restoreTabs() {
        if (m_settings.savedTabs.isEmpty())
        return false;
    // Snapshot: every add below saves (stageTabs clears + rebuilds
    // savedTabs), so iterating the live list is use-after-free - the
    // once-per-version restore segfault.
    const QList<AppSettings::SavedTab> saved = m_settings.savedTabs;
    bool any = false;
    m_tabBar->blockSignals(true);
    int current = 0;
    int n = 0;
    for (const AppSettings::SavedTab &st : saved) {
        if (n >= 25)
            break;
        if (st.kind == QStringLiteral("local")) {
            if (!QDir(st.root).exists())
                continue;
            addLocalTab(st.root);
        } else if (st.kind == QStringLiteral("browse")) {
            if (st.sites.isEmpty())
                continue;
            addRemoteTab(st.sites, st.tags);
        } else if (st.kind == QStringLiteral("favs")) {
            openFavTab(); // each saved fav tab restores with its own filters
        } else if (st.kind == QStringLiteral("hydrus")) {
            if (!m_settings.hydrusTab)
                continue;
            openHydrusTab();
        } else {
            continue;
        }
        BrowserTab *t = m_tabs.last();
        t->searchQuery = st.query;
        t->rating = st.rating.isEmpty() ? QStringLiteral("all") : st.rating;
        t->category = st.category;
        t->sort = st.sort.isEmpty() ? QStringLiteral("added") : st.sort;
        t->hydrusOnlyMissing = st.hydrusOnlyMissing;
        t->viewerKey = st.viewerKey;
        t->scrollRow = st.scrollRow;
        updateTabTitle(static_cast<int>(m_tabs.size()) - 1);
        // Tab-bar index (+1: tab 0 is welcome).
        if (n == qBound(0, m_settings.savedTabCurrent,
                        saved.size() - 1))
            current = static_cast<int>(m_tabs.size());
        n++;
        any = true;
    }
    m_tabBar->blockSignals(false);
    if (any) {
        // The batch above already moved the bar silently while blocked: if
        // it sits on the target, setCurrentIndex is a no-op and the tab
        // would never activate (stuck on welcome) - drive it directly.
        if (m_tabBar->currentIndex() == current)
            onTabChanged(current);
        else
            m_tabBar->setCurrentIndex(current); // fires once: scans/searches it
    }
    return any;
}

bool MainWindow::isBrowse() const {
    return m_curTab >= 0 && m_curTab < m_tabs.size() && m_tabs[m_curTab]->isRemote;
}

QIcon MainWindow::sitesIcon(const QStringList &sites) const {
    QList<QPixmap> marks;
    for (const QString &key : sites) {
        QPixmap pm;
        if (key == kSrcFavourites)
            pm = UiIcons::heart(UiIcons::favRed(), true).pixmap(16, 16);
        else if (key == kSrcDownloads)
            pm = UiIcons::icon(QStringLiteral("download")).pixmap(16, 16);
        else if (const BooruSite *site = RemoteSource::siteFor(key))
            pm = m_favicons->iconFor(key, site->baseUrl).pixmap(16, 16);
        if (!pm.isNull())
            marks << pm.scaled(16, 16, Qt::IgnoreAspectRatio,
                               Qt::SmoothTransformation);
        if (marks.size() >= 3)
            break;
    }
    if (marks.isEmpty())
        return {};
    if (marks.size() == 1)
        return QIcon(marks.first());
    QPixmap combo(static_cast<int>(marks.size()) * 18 - 2, 16);
    combo.fill(Qt::transparent);
    QPainter painter(&combo);
    for (qsizetype i = 0; i < marks.size(); ++i)
        painter.drawPixmap(static_cast<int>(i) * 18, 0, marks.at(i));
    return QIcon(combo);
}

void MainWindow::onFaviconReady(const QString &key, QIcon icon) {
    // Grid origin badges for this site gain their icon in place.
    const QPixmap fav = icon.pixmap(16, 16);
    if (!fav.isNull() && m_model) {
        for (int r = 0; r < m_model->rowCount(); ++r) {
            QStandardItem *it = m_model->item(r);
            if (it && it->data(Qt::UserRole + 11).toString() == key)
                it->setData(fav, Qt::UserRole + 10);
        }
    }
    // Retitle browse tabs using this site (composite marks may have gained
    // an icon). The sites picker stays text-only for readability.
    for (int i = 0; i < m_tabs.size(); ++i) {
        if (m_tabs[i]->isRemote && m_tabs[i]->sites.contains(key))
            updateTabTitle(i);
    }
}

QStringList MainWindow::siteDisplayNames(const QStringList &sites) const {
    QStringList shown;
    for (const QString &key : sites) {
        if (key == kSrcFavourites)
            shown << tr("Favourites");
        else if (key == kSrcDownloads)
            shown << tr("Downloads");
        else {
            // Labels, not slug keys: custom boorus are keyed
            // "newbooru"/"newbooru2" but named by the user.
            const BooruSite *site = RemoteSource::siteFor(key);
            shown << (site ? site->label : key);
        }
    }
    return shown;
}

void MainWindow::updateDots() {
    const auto shown = [](QLabel *l) {
        return l && l->isVisible() && !l->text().trimmed().isEmpty();
    };
    // A dot only separates two shown strings - never dangles at an end
    // or next to an empty label.
    if (m_dotPage)
        m_dotPage->setVisible(shown(m_pageLabel) && shown(m_resultLabel));
    if (m_dotCount)
        m_dotCount->setVisible(shown(m_resultLabel) && shown(m_countLabel));
}

void MainWindow::updateTabTitle(int tabIdx) {
    if (tabIdx < 0 || tabIdx >= m_tabs.size())
        return;
    BrowserTab *t = m_tabs[tabIdx];
    QStringList tags = t->activeTags + t->searchQuery.split(QChar::Space, Qt::SkipEmptyParts);
    tags.removeDuplicates();
    if (t->isFavourites) {
        t->title = tr("Favourites");
        if (!tags.isEmpty())
            t->title += QStringLiteral(" · ") + tags.join(u' ');
        // Solid red heart icon carries the mark (a glyph in the title
        // would double it and can't be colored).
        m_tabBar->setTabIcon(tabIdx + 1,
                              UiIcons::heart(UiIcons::favRed(), true));
    } else if (t->isHydrus) {
        t->title = tr("Hydrus");
        if (!tags.isEmpty())
            t->title += QStringLiteral(" · ") + tags.join(u' ');
        m_tabBar->setTabIcon(tabIdx + 1,
                              UiIcons::icon(QStringLiteral("hydrus")));
    } else if (t->isRemote) {
        const QStringList shown = siteDisplayNames(t->sites);
        t->title = tags.isEmpty() ? shown.join(QStringLiteral(" + ")) : tags.join(u' ');
        m_tabBar->setTabIcon(tabIdx + 1, sitesIcon(t->sites));
    } else {
        t->title = archiveLabelForRoot(m_settings, t->root);
        if (!tags.isEmpty())
            t->title += QStringLiteral(" · ") + tags.join(u' ');
        m_tabBar->setTabIcon(tabIdx + 1,
                             UiIcons::icon(QStringLiteral("archive")));
    }
    m_tabBar->setFullTitle(tabIdx + 1, t->title);
    // Keep the OS title in lockstep when this is the visible tab.
    if (tabIdx == m_curTab) {
        if (t->isFavourites)
            setWindowTitle(QStringLiteral("Box - Favourites"));
        else if (t->isHydrus)
            setWindowTitle(QStringLiteral("Box - Hydrus"));
        else
            setWindowTitle(QStringLiteral("Box - ") + t->title);
    }
}

void MainWindow::addLocalTab(const QString &root) {
    auto *t = new BrowserTab();
    t->isRemote = false;
    t->root = root;
    m_tabs.append(t);
    m_tabBar->addTab(QString());
    updateTabTitle(static_cast<int>(m_tabs.size()) - 1);
    m_tabBar->setCurrentIndex(static_cast<int>(m_tabs.size())); // +1: tab 0 is welcome
    saveTabsNow();
}

void MainWindow::addRemoteTab(const QStringList &sites, const QStringList &tags) {
    auto *t = new BrowserTab();
    t->isRemote = true;
    t->sites = sites.isEmpty() ? m_settings.browseSites : sites;
    t->activeTags = tags;
    t->page = 1;
    m_tabs.append(t);
    m_tabBar->addTab(QString());
    updateTabTitle(static_cast<int>(m_tabs.size()) - 1);
    m_tabBar->setCurrentIndex(static_cast<int>(m_tabs.size()));
    saveTabsNow();
}

void MainWindow::onTabChanged(int index) {
    // Reorder drags move the selected tab and emit currentChanged at every
    // step; that is not a user tab switch, so never reload the tab content
    // (the active tab and its filters are unchanged).
    if (m_tabBar && m_tabBar->isDragging())
        return;
    m_suggester->cancel();
    m_completer->popup()->hide();
    saveCurrentTab();
    if (index <= 0) {
        m_curTab = -1;
        showWelcome();
        saveTabs();
        return;
    }
    activateTab(index - 1);
    saveTabs();
}

void MainWindow::onTabClosed(int index) {
    if (index <= 0)
        return; // welcome tab stays
    const int tabIdx = index - 1;
    if (tabIdx < 0 || tabIdx >= m_tabs.size())
        return;
    const bool wasCurrent = (tabIdx == m_curTab);
    if (wasCurrent)
        saveCurrentTab(); // newest filters/open post land in the reopened copy
    // Browser-style reopen history (Ctrl+Shift+T). Keep the tab's shape and
    // filters; the result rows are dropped so activation rescans/re-searches
    // instead of pinning old copies of the archive/fetched pages.
    BrowserTab *closed = new BrowserTab(*m_tabs[tabIdx]);
    closed->localPosts.clear();
    closed->browseMerged.clear();
    m_closedTabs.prepend(closed);
    while (m_closedTabs.size() > 20)
        delete m_closedTabs.takeLast();
    if (m_searchTab == m_tabs[tabIdx]) {
        // The closed tab owned the in-flight search: its completion
        // handler derefs m_searchTab, so cancel the sources and clear
        // the ownership first (otherwise a late reply is a UAF).
        for (auto *src : m_sources)
            src->cancel();
        m_pendingSites.clear();
        m_searchTab = nullptr;
        m_pendingSearches = 0;
        m_loadingPage = false;
    }
    delete m_tabs.takeAt(tabIdx);
    m_tabBar->blockSignals(true);
    m_tabBar->removeTab(index);
    // Re-sync full titles (indexes shifted).
    for (int i = 0; i < m_tabs.size(); ++i)
        updateTabTitle(i);
    m_tabBar->blockSignals(false);
    if (wasCurrent) {
        if (m_tabs.isEmpty()) {
            // All tabs closed: keep the last-place LABEL around so the
            // home button stays visible, but mark it non-resumable. The
            // next save drops the stale last/* keys, and clicking resume
            // warns instead of resurrecting the closed tab.
            m_settings.hasLastPlace = false;
            // setCurrentIndex(0) would be a no-op here (Qt already selected
            // home while signals were blocked), leaving the stage stuck on
            // the closed tab: go home explicitly.
            showWelcome();
        } else {
            activateTab(qMin(tabIdx, static_cast<int>(m_tabs.size()) - 1));
        }
    } else if (m_curTab > tabIdx) {
        m_curTab--;
    }
    saveTabsNow();
}

void MainWindow::closeCurrentTab() {
    if (m_curTab < 0)
        return; // home/welcome: nothing to close
    if (m_viewerFullscreen)
        setViewerFullscreen(false);
    onTabClosed(m_curTab + 1); // tab-bar index 0 is welcome
}

void MainWindow::openDefaultTab() {
    // Same source choice as the home "New browse tab": the explicit
    // Default tab booru wins, else the default-boorus checklist.
    QStringList sites = m_settings.browseSites;
    if (RemoteSource::siteFor(m_settings.tabsDefault) != nullptr)
        sites = {m_settings.tabsDefault};
    if (sites.isEmpty()) {
        statusBar()->showMessage(
            tr("No boorus configured - pick one in Preferences → Browse"),
            5000);
        return;
    }
    addRemoteTab(sites, {});
}

void MainWindow::reopenClosedTab() {
    while (!m_closedTabs.isEmpty()) {
        BrowserTab *t = m_closedTabs.takeFirst();
        if (!t->isRemote && !t->isFavourites && !t->isHydrus &&
            (t->root.isEmpty() || !QDir(t->root).exists())) {
            const QString root = t->root;
            delete t; // folder vanished while the tab sat in history
            statusBar()->showMessage(
                tr("Cannot reopen %1 - folder no longer exists").arg(root),
                5000);
            continue;
        }
        m_tabs.append(t);
        m_tabBar->addTab(QString());
        updateTabTitle(static_cast<int>(m_tabs.size()) - 1);
        m_tabBar->setCurrentIndex(static_cast<int>(m_tabs.size())); // +1: tab 0 is welcome
        saveTabsNow();
        return;
    }
    statusBar()->showMessage(tr("No recently closed tabs"), 3000);
}

void MainWindow::onTabMoved(int from, int to) {
    // Bar index 0 is the welcome tab: it stays put (every mapping
    // assumes index 0 == home), so drags touching it bounce back.
    if (from == 0 || to == 0) {
        m_tabBar->blockSignals(true);
        if (m_tabBar->count() > 1) {
            if (from == 0 && to > 0 && to < m_tabBar->count())
                m_tabBar->moveTab(to, 0);
            else if (to == 0 && from > 0 && from < m_tabBar->count())
                m_tabBar->moveTab(0, from);
        }
        // The bounce-back moveTab was silenced above, but the first
        // drag already reordered the bar titles (including slot 0,
        // which updateTabTitle never touches): re-assign every
        // title so the bar and m_tabs agree again.
        m_tabBar->setFullTitle(0, tr("Home"));
        for (int i = 0; i < m_tabs.size(); ++i)
            updateTabTitle(i);
        m_tabBar->setCurrentIndex(m_curTab < 0 ? 0 : m_curTab + 1);
        m_tabBar->blockSignals(false);
        saveTabsNow();
        return;
    }
    const int a = from - 1, b = to - 1;
    if (a < 0 || b < 0 || a >= m_tabs.size() || b >= m_tabs.size() ||
        a == b)
        return;
    m_tabs.move(a, b);
    if (m_curTab == a) {
        m_curTab = b;
    } else if (a < b) {
        if (m_curTab > a && m_curTab <= b)
            m_curTab--;
    } else if (m_curTab >= b && m_curTab < a) {
        m_curTab++;
    }
    // The bar moved its own text/icons/tooltips with the tab; only
    // the data order (and the session save) needs updating here.
    saveTabsNow();
}

void MainWindow::activateTab(int tabIdx) {
    if (tabIdx < 0 || tabIdx >= m_tabs.size())
        return;
    // Tab switch: leave the outgoing tab's post state alone so switching
    // back mid-view reopens the same post.
    backToGrid(true);
    // backToGrid() re-showed the OUTGOING tab's filter pills (they still
    // live in m_pills). The target tab's filters are rebuilt by the
    // deferred finishActivateTab, so keep the row hidden for the loading
    // window; otherwise the previous tab's tags flash over the veil.
    if (m_pillBar)
        m_pillBar->setVisible(false);
    m_curTab = tabIdx;
    BrowserTab *t = m_tabs[tabIdx];
    // Window title must switch with the tab bar, not after the deferred
    // scan/search (that read as a stale title for a second on every switch).
    updateTabTitle(tabIdx);
    if (t->isFavourites)
        setWindowTitle(QStringLiteral("Box - Favourites"));
    else if (t->isHydrus)
        setWindowTitle(QStringLiteral("Box - Hydrus"));
    else if (t->isRemote)
        setWindowTitle(QStringLiteral("Box - ") +
                       (t->title.isEmpty() ? tr("Browse") : t->title));
    else
        setWindowTitle(QStringLiteral("Box - ") + t->root);
    m_tabBar->blockSignals(true);
    m_tabBar->setCurrentIndex(tabIdx + 1);
    m_tabBar->blockSignals(false);
    m_stage->setCurrentIndex(1);
    // Restore shared UI from the tab.
    m_activeTags = t->activeTags;
    m_searchQuery = t->searchQuery;
    m_searchEdit->setText(t->searchQuery);
    m_rating = t->rating;
    m_category = t->category;
    m_sort = t->sort;
    syncMediaBox(); // media filter state lives in the tab's tags
    m_sortBox->blockSignals(true);
    m_sortBox->setCurrentIndex(qMax(0, m_sortBox->findData(m_sort)));
    m_sortBox->blockSignals(false);
    setBrowseUi(t->isRemote);
    // Sync the shared browse buffer now: saveTabs()/stageTabs() runs
    // synchronously inside onTabChanged, before the deferred
    // finishActivateTab. Without this, a fresh remote tab inherits the
    // previous tab's browseMerged via saveCurrentTab and never searches.
    if (t->isRemote)
        m_browseMerged = t->browseMerged;
    else
        m_browseMerged.clear();
    // Skeleton first: the tab bar + stage paint NOW; the DB/scan/filter
    // rebuild runs on the next event-loop turn so no switch ever blocks
    // on it. Superseded switches abort in finishActivateTab.
    m_current.clear();
    m_model->clear();
    // The sidebar must not keep showing the previous tab's tags while the
    // deferred rebuild runs: swap in the placeholder now (any rebuild
    // clears it).
    setSidebarLoading(true);
    m_scanBar->setRange(0, 0);
    m_scanBar->setVisible(true);
    // Name the target in the grid area and paint it synchronously, so
    // the click is visibly honoured before the deferred work starts.
    QString busy;
    if (t->isFavourites)
        busy = tr("Loading favourites…");
    else if (t->isHydrus)
        busy = tr("Loading Hydrus…");
    else if (t->isRemote)
        busy = tr("Loading %1…")
                   .arg(t->title.isEmpty() ? tr("browse") : t->title);
    else
        busy = tr("Loading %1…").arg(archiveLabelForRoot(m_settings, t->root));
    m_gridLoading->showFor(busy);
    // Capture the tab pointer, not the numeric index: closing an
    // unrelated tab before this fires shifts every index after it, and
    // the stale index would finish-activate the wrong tab (or none).
    // indexOf on a closed tab simply misses (pointer compare, no deref).
    BrowserTab *activated = t;
    QTimer::singleShot(0, this, [this, activated] {
        const qsizetype idx = m_tabs.indexOf(activated);
        if (idx < 0 || idx != m_curTab)
            return; // closed, or a newer switch already took over
        finishActivateTab(static_cast<int>(idx));
    });
}

void MainWindow::hideTabLoading(const BrowserTab *owner) {
    if (!m_gridLoading || !m_gridLoading->isVisible())
        return;
    if (owner && owner != currentTab())
        return; // a stale job from a tab the user already left
    m_gridLoading->hide();
}

void MainWindow::finishActivateTab(int tabIdx) {
    if (tabIdx < 0 || tabIdx >= m_tabs.size() || tabIdx != m_curTab)
        return;
    BrowserTab *t = m_tabs[tabIdx];
    // Opening or restoring a remote/favourites/Hydrus tab is an explicit
    // network action: open the gate before any prefetch runs.
    if (t->isHydrus || t->isFavourites || t->isRemote)
        allowNetwork();
    if (t->isRemote) {
        // Remote grids need the browser fallback for walled previews and
        // the site icons now that the user chose to browse.
        ensureWebBackend();
        prefetchFavicons();
    }
    if (t->isHydrus) {
        loadHydrusTab(t);
    } else if (t->isFavourites) {
        relinkFavourites(); // point rows at files already on disk (once)
        // Re-use the rows this tab already holds: refreshFavTabs() re-lists
        // the DB whenever it actually changes (favourite from the
        // grid/viewer, download auto-fav, import, restore), so a plain
        // tab switch must not pay a full decode of every row.
        if (t->localPosts.isEmpty())
            t->localPosts = m_favDb.list();
        // Remote-only favourites use the browser fallback for walled
        // previews/videos. Downloaded favourites have a local file and do
        // not need Chromium, so build it only when something is truly
        // remote (also keeps the first favourites open fast).
        bool needsWeb = false;
        for (const Post &p : t->localPosts) {
            if (!p.isRemote)
                continue;
            if (p.absFile.isEmpty() || !QFileInfo::exists(p.absFile)) {
                needsWeb = true;
                break;
            }
        }
        if (needsWeb)
            ensureWebBackend();
        setWindowTitle(QStringLiteral("Box - Favourites"));
        m_rating = t->rating;
        m_category = t->category;
        m_sort = t->sort;
        // Favourites have no scan of their own: populate the rating items
        // (and the hidden category box) here, otherwise a restored
        // favourites tab only offers "All".
        rebuildCombos();
        m_sortBox->blockSignals(true);
        m_sortBox->setCurrentIndex(qMax(0, m_sortBox->findData(m_sort)));
        m_sortBox->blockSignals(false);
        commitFilter(); // rebuilds rating/category combos, filters, titles
        updateSortBox(); // every order is client-side on the favourites list
        // Cached verdicts apply now; misses fetch in the background and
        // persist their splits so the next open starts categorised.
        maybeClassifyBrowse(t);
        m_scanBar->setVisible(false); // skeleton done (no async work here)
        setSidebarLoading(false);     // filter rebuild above may have raced
        hideTabLoading(t);
    } else if (t->isRemote) {
        maybeClassifyBrowse(t);
        syncSitesMenu();
        m_pageLabel->setText(tr("Page %1").arg(t->page));
        if (t->browseMerged.isEmpty())
            doBrowseSearch();
        else {
            m_browseMerged = t->browseMerged;
            applyBrowseFilter();
            m_scanBar->setVisible(false); // skeleton done
            hideTabLoading(t);
        }
    } else {
        m_root = t->root;
        setWindowTitle(QStringLiteral("Box - %1").arg(m_root));
        const QString canon = QDir(m_root).canonicalPath();
        const QString flat =
            canon.isEmpty() ? QDir(m_root).absolutePath() : canon;
        // Downloads landed here while we were elsewhere: rescan instead
        // of showing the stale listing until F5.
        bool dirty = false;
        for (auto it = m_dirtyRoots.begin(); it != m_dirtyRoots.end();) {
            if (*it == flat || flat.startsWith(*it + u'/') ||
                it->startsWith(flat + u'/')) {
                dirty = true;
                it = m_dirtyRoots.erase(it);
            } else {
                ++it;
            }
        }
        if (dirty || t->localPosts.isEmpty()) {
            startScan(m_root);
        } else {
            m_index.setPosts(t->localPosts);
            rebuildCombos();
            rebuildSidebar();
            applyFilter();
            m_scanBar->setVisible(false); // skeleton done
            hideTabLoading(t);
        }
    }
    updateSortBox();
    updateTabTitle(tabIdx);
    // Restore the open post (X tag looking at Y post): identity match, so
    // refills/reorders don't matter; missing rows stay on the grid. Only
    // when results are already in (async scans/searches keep the grid).
    BrowserTab *tb = (tabIdx >= 0 && tabIdx < m_tabs.size())
                         ? m_tabs[tabIdx]
                         : nullptr;
    bool viewerRestored = false;
    if (tb && !tb->viewerKey.isEmpty() && !m_current.isEmpty()) {
        for (int i = 0; i < m_current.size(); ++i) {
            if (viewerKeyFor(m_current.at(i)) == tb->viewerKey) {
                showViewer(i);
                viewerRestored = true;
                break;
            }
        }
    }
    // Grid position: the viewer jump already centers its post; a grid-only
    // tab returns to the first visible row saved on switch/quit.
    if (!viewerRestored && tb && tb->scrollRow > 0 &&
        tb->scrollRow < m_model->rowCount()) {
        m_grid->scrollTo(m_model->index(tb->scrollRow, 0),
                         QAbstractItemView::PositionAtTop);
    }
}

// ── Browse ───────────────────────────────────────────────────
bool MainWindow::isDownloadsTab() {
    const BrowserTab *t = currentTab();
    if (!t)
        return false;
    // Downloads is only the online-booru virtual source. Local folders are
    // read-only archives and get no downloads media filter.
    return t->isRemote && t->sites.contains(kSrcDownloads);
}

void MainWindow::setBrowseUi(bool browse) {
    const BrowserTab *t = currentTab();
    const bool favs = t && t->isFavourites;
    const bool hydrus = t && t->isHydrus;
    const bool downloads = isDownloadsTab();
    // Rating/sort work client-side on favourites and both downloads views.
    // Real browse tabs keep them for the server-side query/append path.
    const bool showFilters = browse || favs || downloads;
    m_ratingBox->setVisible(showFilters && !hydrus);
    // Source filter: favourites and downloads mix several boorus/folders,
    // so the dropdown earns its place there (plain browse tabs are one
    // source and use the Boorus picker instead).
    m_categoryBox->setVisible((favs || downloads) && !hydrus);
    // Downloads media type: extension-based, so it only makes sense on
    // the online Downloads view (local archives get no media picker).
    if (m_mediaBox)
        m_mediaBox->setVisible(downloads && !hydrus);
    m_sortBox->setVisible(showFilters || hydrus);
    m_sitesBtn->setVisible(browse && !hydrus);
    m_pageLabel->setVisible(browse && !hydrus);
    if (m_hydrusImportBtn)
        m_hydrusImportBtn->setVisible(hydrus);
    if (m_hydrusRefreshBtn)
        m_hydrusRefreshBtn->setVisible(hydrus);
    if (m_hydrusMissingBtn) {
        m_hydrusMissingBtn->setVisible(hydrus);
        if (hydrus) {
            m_hydrusMissingBtn->blockSignals(true);
            m_hydrusMissingBtn->setChecked(t->hydrusOnlyMissing);
            m_hydrusMissingBtn->blockSignals(false);
        }
    }
    if (m_aiBtn)
        m_aiBtn->setVisible(!hydrus);
    m_countLabel->setVisible(hydrus || (!browse && !favs));
    updateDots();
    updateDownloadButton();
    m_countLabel->setVisible(hydrus || (!browse && !favs));
}
