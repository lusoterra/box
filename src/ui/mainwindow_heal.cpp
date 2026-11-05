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

void MainWindow::healLocalPosts(QVector<Post> &posts) {
    for (Post &p : posts) {
        if (p.isRemote || !p.postUrl.isEmpty() || p.id <= 0)
            continue;
        if (Stray::isStrayPath(p.absFile))
            continue;
        const BooruSite *e = RemotePosts::entryForPost(p);
        if (!e)
            continue;
        const QString url = RemotePosts::postPageUrlFor(*e, p.id);
        if (!url.isEmpty())
            p.postUrl = url;
    }
}

bool MainWindow::isPostDownloaded(
    const Post &post,
    const QHash<QPair<QString, qint64>, QString> *favLocal) {
    if (!post.isRemote) {
        // On disk under the download folder counts; plain library
        // folders are archives, not downloads. The file must actually
        // be there: a deleted-but-favourited row is not downloaded.
        if (post.absFile.isEmpty() || !QFileInfo::exists(post.absFile))
            return false;
        const QString dlRoot =
            QDir(effectiveDownloadDir(m_settings)).absolutePath();
        const QString abs = QFileInfo(post.absFile).absolutePath();
        return abs == dlRoot || abs.startsWith(dlRoot + u'/');
    }
    // Remote posts count when a favourite row links a file that is
    // still on disk (re-downloads and moved files heal via relink).
    QHash<QPair<QString, qint64>, QString> fresh;
    if (!favLocal) {
        if (!m_favDb.isOpen())
            return false;
        fresh = m_favDb.localPaths();
        favLocal = &fresh;
    }
    const auto it = favLocal->find(favIdentity(post));
    return it != favLocal->end() && !it.value().isEmpty() &&
           QFileInfo::exists(it.value());
}

void MainWindow::updateViewerBadges() {
    if (!m_viewer || !m_viewer->hasCurrent())
        return;
    const Post p = m_viewer->currentPost();
    const auto [site, pid] = favIdentity(p);
    const bool faved =
        m_favDb.isOpen() && m_favDb.isFavourite(site, pid);
    const bool downloaded = isPostDownloaded(p, nullptr);
    m_viewer->setStatusBadges(isAiPost(p), faved, downloaded);
    m_viewer->setFaved(faved);
    m_viewer->setDownloaded(downloaded);
}

void MainWindow::syncViewerToGrid() {
    if (!m_viewer || m_pages->currentIndex() != 1 ||
        !m_viewer->hasCurrent())
        return;
    const auto ident = favIdentity(m_viewer->currentPost());
    int found = -1;
    for (int i = 0; i < m_current.size(); ++i) {
        if (favIdentity(m_current.at(i)) == ident) {
            found = i;
            break;
        }
    }
    if (found == m_viewer->currentRow())
        return; // same post, same slot: leave playback alone
    if (m_current.isEmpty()) {
        backToGrid(); // nothing left to show
        return;
    }
    const int row = found >= 0 ? found
                              : qMin(m_viewer->currentRow(), m_current.size() - 1);
    m_viewer->setPosts(m_current);
    m_viewer->showRow(row);
    const Post &p = m_current.at(row);
    const auto [site, pid] = favIdentity(p);
    m_viewer->setFaved(m_favDb.isOpen() && m_favDb.isFavourite(site, pid));
    const QString reason = refreshReason(p);
    m_viewer->setRefreshable(reason.isEmpty(), reason);
    updateViewerBadges();
}

