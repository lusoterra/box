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

void MainWindow::closeEvent(QCloseEvent *e) {
    // Fast close (X / Alt+Q): unmap and blank the UI first so the window
    // manager responds instantly, then tear down workers without ever
    // blocking this event. Destruction-time waits (pool drains, player
    // shutdown) happen after the window is already gone.
    e->accept();
    hide();
    if (m_stage)
        m_stage->setCurrentIndex(0); // release media widgets, show home
    if (m_viewer)
        m_viewer->stopPlayback();
    cancelScan();
    m_downloads->cancel();
    m_fetcher->cancel();
    m_browseFetcher->cancel();
    m_suggester->cancel();
    if (m_refresher)
        m_refresher->cancel();
    for (RemoteSource *s : m_sources) {
        if (s)
            s->cancel();
    }
    if (m_thumbs)
        m_thumbs->cancel(); // drop queued decodes (pool wait is bounded)
    if (m_videoThumbs)
        m_videoThumbs->cancel();
    m_settings.geometry = saveGeometry();
    if (m_sideSplit)
        m_settings.sideSplitter = m_sideSplit->saveState();
    flushSavedTabs(); // includes saveSettings
    QMainWindow::closeEvent(e);
}

bool MainWindow::eventFilter(QObject *o, QEvent *e) {
    if (o == statusBar() && e->type() == QEvent::MouseButtonPress &&
        m_statusProbeValid) {
        RemotePosts::showProbeDialog(this, m_statusProbeTitle, m_statusProbeText);
        return true;
    }
    return QMainWindow::eventFilter(o, e);
}

void MainWindow::clearStatusProbe() {
    m_statusProbeValid = false;
    m_statusProbeTitle.clear();
    m_statusProbeText.clear();
    statusBar()->setCursor(Qt::ArrowCursor);
    statusBar()->setToolTip(QString());
}

void MainWindow::setStatusProbe(const QString &title, const QString &text) {
    m_statusProbeTitle = title;
    m_statusProbeText = text;
    m_statusProbeValid = true;
    statusBar()->setCursor(Qt::PointingHandCursor);
    statusBar()->setToolTip(tr("Click to see the full server response"));
}

void MainWindow::onGridActivated(const QModelIndex &index) {
    if (!index.isValid())
        return;
    showViewer(index.row());
}

void MainWindow::downloadSelected() {
    const BrowserTab *t = currentTab();
    if (!isBrowse() && !(t && t->isFavourites))
        return;
    QVector<Post> posts;
    for (const QModelIndex &i : m_grid->selectionModel()->selectedRows()) {
        if (i.row() >= 0 && i.row() < m_current.size())
            posts.append(m_current.at(i.row()));
    }
    if (posts.isEmpty()) {
        statusBar()->showMessage(tr("Select posts first"), 3000);
        return;
    }
    // Explicit download lifts the veto (see m_unfavVeto).
    for (const Post &p : posts) {
        const auto [site, pid] = favIdentity(p);
        m_unfavVeto.remove(favVetoKey(site, pid));
    }
    downloadPosts(posts);
}

void MainWindow::updateDownloadButton() {
    const int n = m_grid && m_grid->selectionModel()
                      ? m_grid->selectionModel()->selectedRows().size()
                      : 0;
    m_downloadSelBtn->setText(tr("Download (%1)").arg(n));
    const BrowserTab *t = currentTab();
    const bool downloadable = isBrowse() || (t && t->isFavourites);
    // Always visible where downloads apply (disabled when empty): hiding
    // it on Select/selection changes shuffles the whole omnibar.
    m_downloadSelBtn->setVisible(downloadable);
    m_downloadSelBtn->setEnabled(n > 0);
}

void MainWindow::searchVariants(const Post &post) {
    if (!post.isRemote || post.id <= 0)
        return;
    qint64 parentId = 0;
    for (const char *key : {"parent_id", "parent"}) {
        const QJsonValue v = post.rawJson.value(QLatin1String(key));
        if (v.isNull() || v.isUndefined())
            continue;
        parentId = v.toVariant().toLongLong();
        if (parentId > 0)
            break;
    }
    QStringList alts = {QStringLiteral("parent:") + QString::number(post.id),
                        QStringLiteral("child:") + QString::number(post.id),
                        QStringLiteral("id:") + QString::number(post.id)};
    if (parentId > 0) {
        alts << QStringLiteral("parent:") + QString::number(parentId)
             << QStringLiteral("id:") + QString::number(parentId);
    }
    QStringList unique;
    for (const QString &a : std::as_const(alts))
        if (!unique.contains(a))
            unique << a;
    // Family only: drop old tag cards, keep the tab's rating/blacklist.
    m_activeTags.clear();
    m_searchQuery = canonicalQuery({}, {unique});
    m_searchEdit->clear();
    backToGrid();
    commitSearch();
    statusBar()->showMessage(tr("Showing variants of #%1").arg(post.id), 5000);
}

void MainWindow::addTagToSearch(const QString &tag) {
    if (!m_activeTags.contains(tag))
        m_activeTags.append(tag);
    if (isBrowse()) {
        commitSearch();
    } else {
        rebuildSidebar();
        applyFilter();
    }
    backToGrid();
}

void MainWindow::showViewerTagMenu(const QString &tag, const QPoint &globalPos) {
    QMenu menu;
    const bool fav = isFavTag(tag);
    QAction *favAct = menu.addAction(
        fav ? tr("★ Remove from favourite tags")
            : tr("★ Add to favourite tags"));
    connect(favAct, &QAction::triggered, this,
            [this, tag] { toggleFavTag(tag); });
    menu.addSeparator();
    QAction *addAct = menu.addAction(tr("Add to search"));
    connect(addAct, &QAction::triggered, this,
            [this, tag] { addTagToSearch(tag); });
    QMenu *newTab = menu.addMenu(tr("Search in new tab"));
    // One entry per configured booru: opens a fresh browse tab limited to
    // that site with the tag already filled in.
    for (const BooruSite &site : m_settings.boorus) {
        if (!site.enabled || site.key.isEmpty())
            continue;
        const QString label =
            site.label.isEmpty() ? site.key : site.label;
        QAction *a = newTab->addAction(label);
        connect(a, &QAction::triggered, this, [this, key = site.key, tag] {
            addRemoteTab({key}, {tag});
        });
    }
    if (newTab->isEmpty())
        newTab->setEnabled(false);
    menu.exec(globalPos);
}

