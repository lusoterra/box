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

void MainWindow::refreshFavTabs() {
    if (!m_favDb.isOpen())
        return;
    const QVector<Post> favs = m_favDb.list();
    for (int i = 0; i < m_tabs.size(); ++i) {
        if (!m_tabs[i]->isFavourites)
            continue;
        m_tabs[i]->localPosts = favs;
        updateTabTitle(i);
    }
    // The visible tab re-filters too. This used to activateTab(),
    // which tears the viewer down mid-look (favourite/download from
    // the post view kicked back to the grid); instead rebuild the
    // data in place and let fillModel reconcile the viewer.
    if (BrowserTab *t = currentTab(); t && t->isFavourites) {
        if (m_pages->currentIndex() == 1)
            commitFilter();
        else
            activateTab(m_curTab);
    }
    updateViewerBadges();
}

QString MainWindow::describeLastPlace() const {
    // Deliberately ignores hasLastPlace: after the last tab is closed the
    // label stays visible so clicking it can explain that there is nothing
    // left to resume (the persisted last/* keys are already dropped).
    if (m_settings.lastPlace.kind.isEmpty())
        return {};
    const AppSettings::SavedTab &lp = m_settings.lastPlace;
    if (lp.kind == QStringLiteral("favs"))
        return tr("Favourites");
    if (lp.kind == QStringLiteral("browse")) {
        const QStringList shown = siteDisplayNames(lp.sites);
        const QString where = lp.tags.isEmpty()
                                  ? shown.join(QStringLiteral(" + "))
                                  : lp.tags.join(u' ');
        return where.isEmpty() ? tr("Browse") : where;
    }
    const QString name = QDir(lp.root).dirName();
    return name.isEmpty() ? lp.root : name;
}

void MainWindow::startFreshSession() {
    // Close everything; the shrinking saves already empty the persisted
    // session, and the last place is cleared so resume stays hidden.
    while (m_tabBar->count() > 1)
        onTabClosed(1);
    m_settings.hasLastPlace = false;
    m_settings.lastPlace = AppSettings::SavedTab();
    m_settings.savedTabs.clear();
    m_settings.savedTabCurrent = 0;
    saveSettings(m_settings);
    showWelcome();
}

void MainWindow::openLastPlace() {
    // Last session first: restore every saved tab, landing on the saved
    // current one inside restoreTabs. Single-tab fallback below when
    // nothing was ever saved.
    if (m_tabs.isEmpty()) {
        if (restoreTabs())
            return;
    } else {
        const int idx =
            qBound(0, m_settings.savedTabCurrent, m_tabs.size() - 1);
        if (m_tabBar->currentIndex() == idx + 1)
            activateTab(idx); // already there: refresh in place
        else
            m_tabBar->setCurrentIndex(idx + 1); // +1: tab 0 is welcome
        return;
    }
    if (!m_settings.hasLastPlace || m_settings.lastPlace.kind.isEmpty()) {
        // Everything from the previous session was closed (or the stale
        // place did not survive): say so instead of silently doing nothing.
        QMessageBox::information(
            this, tr("Nothing to resume"),
            tr("The previous session has no open tabs left to return to."));
        m_welcome->setLastPlace({});
        return;
    }
    const AppSettings::SavedTab lp = m_settings.lastPlace;
    // Reuse the tab when it is still open (keeps its live state).
    if (lp.kind == QStringLiteral("favs")) {
        int favIdx = -1;
        for (int i = 0; i < m_tabs.size(); ++i) {
            if (m_tabs[i]->isFavourites) {
                favIdx = i;
                break;
            }
        }
        if (favIdx < 0) {
            auto *t = new BrowserTab();
            t->isFavourites = true;
            t->sort = QStringLiteral("added"); // DB order is newest-first
            t->title = tr("Favourites");
            m_tabs.append(t);
            m_tabBar->addTab(QString());
            updateTabTitle(m_tabs.size() - 1);
            favIdx = m_tabs.size() - 1;
        }
        BrowserTab *t = m_tabs[favIdx];
        t->activeTags = lp.tags;
        t->searchQuery = lp.query;
        t->rating = lp.rating;
        t->sort = lp.sort;
        t->viewerKey = lp.viewerKey;
        t->scrollRow = lp.scrollRow;
        if (m_tabBar->currentIndex() == favIdx + 1)
            activateTab(favIdx); // already there: refresh in place
        else
            m_tabBar->setCurrentIndex(favIdx + 1);
        saveTabsNow();
        return;
    }
    if (lp.kind == QStringLiteral("browse") && !lp.sites.isEmpty()) {
        for (int i = 0; i < m_tabs.size(); ++i) {
            BrowserTab *t = m_tabs[i];
            if (t->isRemote && !t->isFavourites && t->sites == lp.sites) {
                t->activeTags = lp.tags;
                t->searchQuery = lp.query;
                t->rating = lp.rating;
                t->sort = lp.sort;
                t->viewerKey = lp.viewerKey;
                t->scrollRow = lp.scrollRow;
                if (m_tabBar->currentIndex() == i + 1)
                    activateTab(i); // already there: refresh in place
                else
                    m_tabBar->setCurrentIndex(i + 1);
                return;
            }
        }
        auto *t = new BrowserTab();
        t->isRemote = true;
        t->sites = lp.sites;
        t->activeTags = lp.tags;
        t->searchQuery = lp.query;
        t->rating = lp.rating;
        t->sort = lp.sort;
        t->viewerKey = lp.viewerKey;
        t->scrollRow = lp.scrollRow;
        t->page = 1;
        m_tabs.append(t);
        m_tabBar->addTab(QString());
        updateTabTitle(m_tabs.size() - 1);
        m_tabBar->setCurrentIndex(m_tabs.size()); // +1: tab 0 is welcome
        saveTabsNow();
        return;
    }
    if (lp.kind == QStringLiteral("local") && !lp.root.isEmpty()) {
        if (!QDir(lp.root).exists()) {
            statusBar()->showMessage(
                tr("Last place is gone: %1").arg(lp.root), 6000);
            return;
        }
        for (int i = 0; i < m_tabs.size(); ++i) {
            BrowserTab *t = m_tabs[i];
            if (!t->isRemote && !t->isFavourites && t->root == lp.root) {
                t->activeTags = lp.tags;
                t->searchQuery = lp.query;
                t->rating = lp.rating;
                t->category = lp.category;
                t->sort = lp.sort;
                t->viewerKey = lp.viewerKey;
                t->scrollRow = lp.scrollRow;
                if (m_tabBar->currentIndex() == i + 1)
                    activateTab(i); // already there: refresh in place
                else
                    m_tabBar->setCurrentIndex(i + 1);
                return;
            }
        }
        auto *t = new BrowserTab();
        t->isRemote = false;
        t->root = lp.root;
        t->activeTags = lp.tags;
        t->searchQuery = lp.query;
        t->rating = lp.rating;
        t->category = lp.category;
        t->sort = lp.sort;
        t->viewerKey = lp.viewerKey;
        t->scrollRow = lp.scrollRow;
        m_tabs.append(t);
        m_tabBar->addTab(QString());
        updateTabTitle(m_tabs.size() - 1);
        m_tabBar->setCurrentIndex(m_tabs.size()); // +1: tab 0 is welcome
        saveTabsNow();
    }
}

