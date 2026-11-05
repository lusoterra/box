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

void MainWindow::saveCurrentTab() {
    if (m_curTab < 0 || m_curTab >= m_tabs.size())
        return;
    BrowserTab *t = m_tabs[m_curTab];
    t->activeTags = m_activeTags;
    t->searchQuery = m_searchQuery;
    t->rating = m_rating;
    t->category = m_category;
    t->sort = m_sort;
    int lo = 0, hi = -1;
    visibleRowRange(lo, hi);
    if (hi >= 0)
        t->scrollRow = lo; // keep the old position if the model was empty
    if (!t->isRemote) {
        if (!t->isFavourites)
            t->localPosts = m_index.posts();
        // else: favourites list is DB-owned (re-listed on activation);
        // overwriting it here is what made new favourites vanish on tab
        // switches until the next manual refresh.
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
        lp.kind = c->isFavourites ? QStringLiteral("favs")
                                  : (c->isRemote ? QStringLiteral("browse")
                                                 : QStringLiteral("local"));
        lp.root = c->isRemote ? QString() : c->root;
        lp.sites = c->isRemote ? c->sites : QStringList();
        lp.tags = c->activeTags;
        lp.query = c->searchQuery;
        lp.rating = c->rating;
        lp.category = c->category;
        lp.sort = c->sort;
        lp.viewerKey = c->viewerKey;
        lp.scrollRow = c->scrollRow;
    }
    m_settings.savedTabs.clear();
    for (BrowserTab *t : m_tabs) {
        if (!t->isRemote && !t->isFavourites)
            continue; // local archives never persist (library untouched)
        AppSettings::SavedTab st;
        if (t->isFavourites)
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
        } else {
            continue;
        }
        BrowserTab *t = m_tabs.last();
        t->searchQuery = st.query;
        t->rating = st.rating.isEmpty() ? QStringLiteral("all") : st.rating;
        t->category = st.category;
        t->sort = st.sort.isEmpty() ? QStringLiteral("id-desc") : st.sort;
        t->viewerKey = st.viewerKey;
        t->scrollRow = st.scrollRow;
        updateTabTitle(m_tabs.size() - 1);
        if (n == qBound(0, m_settings.savedTabCurrent,
                        saved.size() - 1))
            current = m_tabs.size(); // tab-bar index (+1: tab 0 is welcome)
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
    QPixmap combo(marks.size() * 18 - 2, 16);
    combo.fill(Qt::transparent);
    QPainter painter(&combo);
    for (int i = 0; i < marks.size(); ++i)
        painter.drawPixmap(i * 18, 0, marks.at(i));
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
        // Solid red heart icon carries the mark (a ♥ glyph in the title
        // would double it and can't be colored).
        m_tabBar->setTabIcon(tabIdx + 1,
                              UiIcons::heart(UiIcons::favRed(), true));
    } else if (t->isRemote) {
        const QStringList shown = siteDisplayNames(t->sites);
        t->title = tags.isEmpty() ? shown.join(QStringLiteral(" + ")) : tags.join(u' ');
        m_tabBar->setTabIcon(tabIdx + 1, sitesIcon(t->sites));
    } else {
        t->title = QDir(t->root).dirName();
        if (t->title.isEmpty())
            t->title = t->root;
        // The download folder is literally named "downloads": show it
        // like everywhere else instead of the raw dirname.
        if (t->title.compare(QStringLiteral("downloads"), Qt::CaseInsensitive) == 0)
            t->title = tr("Downloads");
        if (!tags.isEmpty())
            t->title += QStringLiteral(" · ") + tags.join(u' ');
        m_tabBar->setTabIcon(tabIdx + 1, UiIcons::icon(QStringLiteral("folder-open")));
    }
    m_tabBar->setFullTitle(tabIdx + 1, t->title);
    // Keep the OS title in lockstep when this is the visible tab.
    if (tabIdx == m_curTab) {
        if (t->isFavourites)
            setWindowTitle(QStringLiteral("Box - ♥ Favourites"));
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
    updateTabTitle(m_tabs.size() - 1);
    m_tabBar->setCurrentIndex(m_tabs.size()); // +1: tab 0 is welcome
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
    updateTabTitle(m_tabs.size() - 1);
    m_tabBar->setCurrentIndex(m_tabs.size());
    saveTabsNow();
}

void MainWindow::onTabChanged(int index) {
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
            activateTab(qMin(tabIdx, m_tabs.size() - 1));
        }
    } else if (m_curTab > tabIdx) {
        m_curTab--;
    }
    saveTabsNow();
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
    backToGrid();
    m_curTab = tabIdx;
    BrowserTab *t = m_tabs[tabIdx];
    // Window title must switch with the tab bar, not after the deferred
    // scan/search (that read as a stale title for a second on every switch).
    updateTabTitle(tabIdx);
    if (t->isFavourites)
        setWindowTitle(QStringLiteral("Box - ♥ Favourites"));
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
    m_scanBar->setRange(0, 0);
    m_scanBar->setVisible(true);
    QTimer::singleShot(0, this, [this, tabIdx] {
        if (tabIdx != m_curTab)
            return; // a newer switch already took over
        finishActivateTab(tabIdx);
    });
}

void MainWindow::finishActivateTab(int tabIdx) {
    if (tabIdx < 0 || tabIdx >= m_tabs.size() || tabIdx != m_curTab)
        return;
    BrowserTab *t = m_tabs[tabIdx];
    if (t->isRemote && !t->isFavourites)
        maybeClassifyBrowse(t);
    if (t->isFavourites) {
        relinkFavourites(); // point rows at files already on disk (once)
        t->localPosts = m_favDb.list();
        setWindowTitle(QStringLiteral("Box - ♥ Favourites"));
        m_rating = t->rating;
        m_category = t->category;
        m_sort = t->sort;
        m_sortBox->blockSignals(true);
        m_sortBox->setCurrentIndex(qMax(0, m_sortBox->findData(m_sort)));
        m_sortBox->blockSignals(false);
        commitFilter(); // rebuilds rating/category combos, filters, titles
        m_scanBar->setVisible(false); // skeleton done (no async work here)
        return;
    }
    if (t->isRemote) {
        syncSitesMenu();
        m_pageLabel->setText(tr("Page %1").arg(t->page));
        if (t->browseMerged.isEmpty())
            doBrowseSearch();
        else {
            m_browseMerged = t->browseMerged;
            applyBrowseFilter();
            m_scanBar->setVisible(false); // skeleton done
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
    if (!viewerRestored && tb && tb->scrollRow >= 0 &&
        tb->scrollRow < m_model->rowCount()) {
        m_grid->scrollTo(m_model->index(tb->scrollRow, 0),
                         QAbstractItemView::PositionAtTop);
    }
}

// ── Browse ───────────────────────────────────────────────────
void MainWindow::setBrowseUi(bool browse) {
    const BrowserTab *t = currentTab();
    const bool favs = t && t->isFavourites;
    m_categoryBox->setVisible(!browse);
    m_sitesBtn->setVisible(browse);
    m_pageLabel->setVisible(browse);
    m_countLabel->setVisible(!browse && !favs);
    updateDots();
    updateDownloadButton();
    m_countLabel->setVisible(!browse && !favs);
}
