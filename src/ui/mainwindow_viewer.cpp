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
    const QModelIndex idx = m_model->index(row, 0);
    m_viewer->showThumbPreview(
        qvariant_cast<QPixmap>(m_model->data(idx, Qt::DecorationRole)));
    m_viewer->setPosts(m_current);
    m_viewer->showRow(row);
    if (row >= 0 && row < m_current.size()) {
        const Post &p = m_current[row];
        const auto [site, pid] = favIdentity(p);
        m_viewer->setFaved(m_favDb.isOpen() && m_favDb.isFavourite(site, pid));
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
    if (on)
        m_preFullscreenState = windowState();
    m_viewerFullscreen = on;
    if (m_viewer)
        m_viewer->setImmersive(on);
    menuBar()->setVisible(!on);
    if (m_tabBar)
        m_tabBar->setVisible(!on);
    if (m_plusBtn)
        m_plusBtn->setVisible(!on);
    if (m_omniBar)
        m_omniBar->setVisible(!on);
    if (m_sideBar)
        m_sideBar->setVisible(!on);
    if (m_pillBar)
        m_pillBar->setVisible(!on && m_pills && m_pills->count() > 0);
    statusBar()->setVisible(!on);
    if (on) {
        showFullScreen();
    } else {
        // Restore the exact state captured on entry through the Qt state
        // API instead of a showMaximized()/showNormal() pair: on several
        // WMs the pair ends up non-maximized.
        setWindowState(m_preFullscreenState);
        m_preFullscreenState = Qt::WindowNoState;
        if (m_pages)
            m_pages->setCurrentIndex(1); // stay on the viewer
        if (m_viewer)
            m_viewer->setFocus();
    }
}

void MainWindow::showViewerContextMenu(const Post &post,
                                       const QPoint &globalPos) {
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
    if (!local.isEmpty() && QFileInfo::exists(local)) {
        QAction *copyPath = menu.addAction(tr("Copy file path"));
        connect(copyPath, &QAction::triggered, this, [local] {
            QApplication::clipboard()->setText(local);
        });
        QAction *copyFile = menu.addAction(tr("Copy file (clipboard)"));
        connect(copyFile, &QAction::triggered, this, [this, post, local] {
            QString path = local;
            if (path.isEmpty()) {
                const QString url = post.fileUrl.isEmpty() ? post.sampleUrl : post.fileUrl;
                if (url.isEmpty())
                    return;
                TempMedia::ensureTemp(url, QStringLiteral("jpg"),
                                     [this](const QString &p) {
                                         if (p.isEmpty())
                                             return;
                                         auto *md = new QMimeData();
                                         md->setUrls({QUrl::fromLocalFile(p)});
                                         QApplication::clipboard()->setMimeData(md);
                                     });
                return;
            }
            auto *md = new QMimeData();
            md->setUrls({QUrl::fromLocalFile(path)});
            QApplication::clipboard()->setMimeData(md);
        });
        QAction *open = menu.addAction(tr("Open file"));
        connect(open, &QAction::triggered, this, [local] {
            QDesktopServices::openUrl(QUrl::fromLocalFile(local));
        });
    }
    if (post.isRemote && hasFileLink) {
        const bool downloaded =
            !m_favDb.localPaths().value(favIdentity(post)).isEmpty();
        QAction *dl = menu.addAction(downloaded ? tr("Delete from disk…")
                                                : tr("⭳ Download"));
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
    QAction *favAction = menu.addAction(fav ? tr("♡ Unfavourite")
                                            : tr("♥ Favourite"));
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

