#include "mainwindow.h"
#include "appinfo.h"
#include "cache.h"
#include "cachejanitor.h"
#include "cachepolicy.h"
#include "cookiegrab.h"
#include "desktopfiles.h"
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
#include "tempmedia.h"
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
#include <QEvent>
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

void MainWindow::showViewer(int row) {
    if (row >= 0 && row < m_current.size())
        if (BrowserTab *t = currentTab())
            t->viewerKey = viewerKeyFor(m_current.at(row));
    const bool hydrusTab = currentTab() && currentTab()->isHydrus;
    // Favourites rows keep isRemote; their downloaded sidecars are only
    // reachable through the DB link. Hand the viewer "site#id" -> path so
    // cached notes work regardless of which tab opened the post. The
    // Hydrus tab is independent: do not even read the favourites DB.
    if (m_viewer && m_favDb.isOpen() && !hydrusTab) {
        QHash<QString, QString> idx;
        const auto paths = m_favDb.localPaths();
        for (auto it = paths.begin(); it != paths.end(); ++it) {
            const QString key = RemotePosts::keyForLabel(it.key().first);
            idx.insert((key.isEmpty() ? it.key().first : key) + u'#' +
                           QString::number(it.key().second),
                       it.value());
        }
        m_viewer->setLocalPathIndex(idx);
    }
    const QModelIndex idx = m_model->index(row, 0);
    m_viewer->showThumbPreview(
        qvariant_cast<QPixmap>(m_model->data(idx, Qt::DecorationRole)));
    m_viewer->setPosts(m_current);
    m_viewer->setHydrusMode(currentTab() && currentTab()->isHydrus);
    m_viewer->showRow(row);
    if (row >= 0 && row < m_current.size()) {
        const Post &p = m_current[row];
        const auto [site, pid] = favIdentity(p);
        m_viewer->setFaved(!hydrusTab && m_favDb.isOpen() &&
                           m_favDb.isFavourite(site, pid));
        updateViewerBadges();
        const QString reason = refreshReason(p);
        m_viewer->setRefreshable(reason.isEmpty(), reason);
    }
    if (m_omniBar)
        m_omniBar->setVisible(false);
    // Pills act on the grid too (same misclick risk as the omnibar).
    if (m_pillBar)
        m_pillBar->setVisible(false);
    m_pages->setCurrentIndex(1);
}

void MainWindow::toggleViewerFullscreen() {
    if (m_pages->currentIndex() != 1) {
        // From the grid: open the selected/current post first, then go
        // fullscreen once the viewer page is live.
        if (m_current.isEmpty())
            return;
        int row = m_grid->currentIndex().row();
        if (row < 0 || row >= m_current.size())
            row = 0;
        showViewer(row);
    }
    setViewerFullscreen(!m_viewerFullscreen);
}

void MainWindow::setViewerFullscreen(bool on) {
    if (m_viewerFullscreen == on)
        return;
    // Capture the window state before touching any chrome: hiding the
    // menu/omnibar/sidebar triggers relayouts that some WMs answer with
    // a geometry state change, and the fullscreen restore must reflect
    // how the window was the moment the user asked for fullscreen.
    if (on) {
        m_preFullscreenState = windowState();
        m_preFullscreenMaximized = isMaximized();
    }
    m_viewerFullscreen = on;
    if (m_viewer)
        m_viewer->setImmersive(on);
    menuBar()->setVisible(!on);
    if (m_tabBar)
        m_tabBar->setVisible(!on);
    if (m_plusBtn)
        m_plusBtn->setVisible(!on);
    // The omnibar/pills only exist on the grid: leaving fullscreen stays
    // on the viewer page (setCurrentIndex(1) below), so restoring them
    // here would put search chrome over the post (backToGrid re-shows
    // them when the grid actually returns).
    const bool onGrid = m_pages && m_pages->currentIndex() == 0;
    if (m_omniBar)
        m_omniBar->setVisible(!on && onGrid);
    if (m_sideBar)
        m_sideBar->setVisible(!on);
    if (m_pillBar)
        m_pillBar->setVisible(!on && onGrid && m_pills &&
                               m_pills->count() > 0);
    statusBar()->setVisible(!on);
    if (on) {
        m_pendingFullscreenMaximize = false;
        showFullScreen();
    } else {
        // Leave fullscreen through the state API (never showNormal(), which
        // drops the maximize flag on several WMs) so the window comes back
        // as it was captured on entry.
        const Qt::WindowStates before = m_preFullscreenState;
        const bool wasMax = m_preFullscreenMaximized;
        m_preFullscreenState = Qt::WindowNoState;
        m_preFullscreenMaximized = false;
        setWindowState(before & ~Qt::WindowFullScreen);
        if (wasMax) {
            // KWin restores the pre-fullscreen *normal* geometry and drops
            // the maximize flag, discarding a maximize asked for in the same
            // pass: a maximized window would come back filling the workspace
            // with a normal frame. changeEvent() re-asks on the compositor
            // configure; this timer is the backstop for the case where no
            // state event follows, and disarms the retry after the transition
            // so a later user unmaximize is never fought.
            m_pendingFullscreenMaximize = true;
            QTimer::singleShot(800, this, [this] {
                if (!m_pendingFullscreenMaximize)
                    return;
                if (!isFullScreen() && !isMaximized())
                    showMaximized();
                m_pendingFullscreenMaximize = false;
            });
        }
        if (m_pages)
            m_pages->setCurrentIndex(1); // stay on the viewer
        if (m_viewer)
            m_viewer->setFocus();
    }
}

void MainWindow::changeEvent(QEvent *e) {
    // The compositor's unfullscreen configure can land after our own state
    // request and reset it back to a normal window at workspace size: re-ask
    // maximize once it has been applied (see setViewerFullscreen).
    if (e->type() == QEvent::WindowStateChange && m_pendingFullscreenMaximize &&
        !isFullScreen() && !isMaximized())
        showMaximized();
    QMainWindow::changeEvent(e);
}

void MainWindow::showViewerContextMenu(const Post &post,
                                       const QPoint &globalPos) {
    if (BrowserTab *ht = currentTab(); ht && ht->isHydrus) {
        showHydrusViewerMenu(post, globalPos);
        return;
    }
    QMenu menu(this);
    QAction *full = menu.addAction(
        m_viewerFullscreen ? tr("Exit fullscreen") : tr("Fullscreen"));
    full->setShortcut(QKeySequence(Qt::Key_F11));
    connect(full, &QAction::triggered, this,
            &MainWindow::toggleViewerFullscreen);
    menu.addSeparator();
    const bool hasPostLink =
        !post.postUrl.isEmpty() && RemotePosts::isHttpUrl(post.postUrl);
    const bool hasFileLink =
        !post.fileUrl.isEmpty() && RemotePosts::isHttpUrl(post.fileUrl);
    const bool hasMediaLink =
        hasFileLink ||
        (!post.sampleUrl.isEmpty() && RemotePosts::isHttpUrl(post.sampleUrl));
    if (hasPostLink) {
        QAction *a = menu.addAction(tr("Open post page"));
        connect(a, &QAction::triggered, this, [url = post.postUrl] {
            QDesktopServices::openUrl(QUrl(url));
        });
        QAction *copyPost = menu.addAction(tr("Copy post link"));
        connect(copyPost, &QAction::triggered, this,
                [url = post.postUrl] {
                    QApplication::clipboard()->setText(url);
                });
    }
    if (hasFileLink) {
        QAction *copyFile = menu.addAction(tr("Copy media link"));
        connect(copyFile, &QAction::triggered, this,
                [url = post.fileUrl] {
                    QApplication::clipboard()->setText(url);
                });
    }
    const QString local =
        !post.absFile.isEmpty() ? post.absFile
                                : m_favDb.localPaths().value(favIdentity(post));
    const bool hasLocal = !local.isEmpty() && QFileInfo::exists(local);
    if (hasLocal) {
        QAction *copyPath = menu.addAction(tr("Copy file path"));
        connect(copyPath, &QAction::triggered, this, [local] {
            QApplication::clipboard()->setText(local);
        });
    }
    if (hasLocal || hasMediaLink) {
        // Favourite-only rows behave like downloaded ones: the media is
        // fetched into the temp folder if no local copy exists.
        QAction *copyFile = menu.addAction(tr("Copy file (clipboard)"));
        connect(copyFile, &QAction::triggered, this, [this, post] {
            copyPostFilesToClipboard({post});
        });
        QAction *open = menu.addAction(tr("Open file"));
        if (hasLocal) {
            connect(open, &QAction::triggered, this, [local] {
                QDesktopServices::openUrl(QUrl::fromLocalFile(local));
            });
        } else {
            connect(open, &QAction::triggered, this, [this, post] {
                m_viewer->openExternally(post);
            });
        }
    }
    QAction *meta = menu.addAction(tr("View metadata…"));
    connect(meta, &QAction::triggered, this,
            [this, post] { showPostMetadata(post); });
    if (m_settings.hydrusTagLookups && (hasLocal || isMd5Hex(post.md5))) {
        QAction *hydrus = menu.addAction(tr("Hydrus tags…"));
        hydrus->setToolTip(
            tr("Read-only: look this file up in Hydrus by content hash"));
        connect(hydrus, &QAction::triggered, this,
                [this, post] { showHydrusTags(post); });
    }
    // Downloaded posts read notes from the sidecar; force an online
    // refresh here (the result is written back when the row is local).
    if (!post.isRemote && RemotePosts::entryForPost(post)) {
        QAction *refreshNotes = menu.addAction(tr("Refresh notes"));
        connect(refreshNotes, &QAction::triggered, this,
                [this] { m_viewer->refreshNotes(); });
    }
    if (hasLocal) {
        QAction *showFolder = menu.addAction(tr("Show in folder"));
        connect(showFolder, &QAction::triggered, this, [local] {
            DesktopFiles::showInFolder(local);
        });
    }
    if (post.isRemote && hasFileLink) {
        const bool downloaded =
            !m_favDb.localPaths().value(favIdentity(post)).isEmpty();
        QAction *dl = menu.addAction(
            downloaded ? UiIcons::icon(QStringLiteral("trash-2"))
                       : UiIcons::icon(QStringLiteral("download")),
            downloaded ? tr("Delete from disk…") : tr("Download"));
        connect(dl, &QAction::triggered, this, [this, post, downloaded] {
            if (!downloaded) {
                const auto [site, pid] = favIdentity(post);
                m_unfavVeto.remove(favVetoKey(site, pid));
                downloadPosts({post});
            } else {
                const QString path = m_favDb.localPaths().value(
                    favIdentity(post));
                if (path.isEmpty() || !QFileInfo::exists(path))
                    return;
                const QFileInfo fi(path);
                QFile::remove(path);
                for (const QString &sp :
                     DownloadManager::sidecarPathsFor(path))
                    QFile::remove(sp);
                invalidateDownloadIndex();
                updateViewerBadges();
                repaintFavMarks();
                statusBar()->showMessage(tr("Deleted %1").arg(fi.fileName()),
                                        4000);
            }
        });
    }
    menu.addSeparator();
    const auto [site, pid] = favIdentity(post);
    const bool fav = m_favDb.isFavourite(site, pid);
    QAction *favAction = menu.addAction(
        UiIcons::heart(UiIcons::favRed(), fav),
        fav ? tr("Unfavourite") : tr("Favourite"));
    connect(favAction, &QAction::triggered, this, [this, post, fav] {
        if (fav) {
            const auto [s, id] = favIdentity(post);
            m_favDb.remove(s, id);
            m_unfavVeto.insert(favVetoKey(s, id));
            refreshFavTabs();
            repaintFavMarks();
            updateViewerBadges();
        } else {
            toggleFavourite(post);
        }
    });
    menu.exec(globalPos);
}

