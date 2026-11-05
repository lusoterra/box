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

void MainWindow::downloadPosts(const QVector<Post> &posts) {
    QVector<Post> remote;
    for (const Post &p : posts) {
        if (p.isRemote && !p.fileUrl.isEmpty())
            remote.append(p);
    }
    if (remote.isEmpty()) {
        statusBar()->showMessage(
            tr("Nothing downloadable (login-walled entries have no file URL)"),
            6000);
        return;
    }
    const QString dest = effectiveDownloadDir(m_settings);
    const QList<DownloadJob> jobs = DownloadManager::plan(remote, dest);
    if (jobs.isEmpty()) {
        statusBar()->showMessage(tr("Nothing to download"), 4000);
        return;
    }
    m_downloads->enqueue(jobs); // no popup: progress lives in the feedbar
}

void MainWindow::showDlDialog() {
    const QString dest = effectiveDownloadDir(m_settings);
    if (!m_dlDialog) {
        m_dlDialog = new DownloadsDialog(m_downloads, dest, this);
        m_dlDialog->setAttribute(Qt::WA_DeleteOnClose, false);
        connect(m_downloads, &DownloadManager::progress, m_dlDialog,
                &DownloadsDialog::onProgress);
        connect(m_downloads, &DownloadManager::fileStarted, m_dlDialog,
                &DownloadsDialog::onFileStarted);
        connect(m_downloads, &DownloadManager::fileFinished, m_dlDialog,
                &DownloadsDialog::onFileFinished);
        connect(m_downloads, &DownloadManager::fileSkipped, m_dlDialog,
                &DownloadsDialog::onFileSkipped);
        connect(m_downloads, &DownloadManager::fileRenamed, m_dlDialog,
                &DownloadsDialog::onFileRenamed);
        connect(m_downloads, &DownloadManager::finished, m_dlDialog,
                &DownloadsDialog::onFinished);
        connect(m_dlDialog, &DownloadsDialog::browseRequested, this,
                &MainWindow::openPath);
    }
    m_dlDialog->show();
    m_dlDialog->raise();
}


// Recorded download links inverted to canonical paths: local scanned
// posts carry ("local", file-hash) identities that never match their
// ("Site", id) favourite rows, so grid fav/delete actions resolve by
// path instead of identity alone. Missing files fall back to absolute
// paths (canonicalization only resolves what is still on disk).
QHash<QString, QList<QPair<QString, qint64>>> MainWindow::favKeysByPath(
    const QHash<QPair<QString, qint64>, QString> &recorded) const {
    QHash<QString, QList<QPair<QString, qint64>>> byCanonPath;
    for (auto it = recorded.begin(); it != recorded.end(); ++it) {
        if (it.value().isEmpty())
            continue;
        QString canon = QFileInfo(it.value()).canonicalFilePath();
        if (canon.isEmpty())
            canon = QFileInfo(it.value()).absoluteFilePath();
        if (!canon.isEmpty())
            byCanonPath[canon].append(it.key());
    }
    return byCanonPath;
}

QList<QPair<QString, qint64>> MainWindow::favKeysFor(
    const Post &p,
    const QHash<QString, QList<QPair<QString, qint64>>> &byPath) const {
    QList<QPair<QString, qint64>> out;
    out.append(favIdentity(p));
    if (!p.absFile.isEmpty()) {
        QString canon = QFileInfo(p.absFile).canonicalFilePath();
        if (canon.isEmpty())
            canon = QFileInfo(p.absFile).absoluteFilePath();
        for (const auto &key : byPath.value(canon)) {
            if (!out.contains(key))
                out.append(key);
        }
    }
    return out;
}

bool MainWindow::isFavMarked(
    const Post &p,
    const QHash<QPair<QString, qint64>, QString> &favLocal,
    const QHash<QString, QList<QPair<QString, qint64>>> &byPath) const {
    if (favLocal.contains(favIdentity(p)))
        return true;
    if (p.absFile.isEmpty())
        return false;
    QString canon = QFileInfo(p.absFile).canonicalFilePath();
    if (canon.isEmpty())
        canon = QFileInfo(p.absFile).absoluteFilePath();
    return byPath.contains(canon);
}

void MainWindow::repaintFavMarks() {
    // Recompute the current tab's ♥/↓ marks in place (bulk fav/unfav must
    // not wait for the next refill) and refresh the viewer badges + fav
    // toggle for whatever post is on screen.
    if (!m_favDb.isOpen() || !m_model)
        return;
    const QHash<QPair<QString, qint64>, QString> favLocal =
        m_favDb.localPaths();
    const auto byPath = favKeysByPath(favLocal);
    for (int row = 0; row < m_model->rowCount() && row < m_current.size();
         ++row) {
        Post &p = m_current[row];
        p.downloaded = isPostDownloaded(p, &favLocal);
        m_model->setData(m_model->index(row, 0),
                         isFavMarked(p, favLocal, byPath),
                         Qt::UserRole + 12);
        m_model->setData(m_model->index(row, 0), p.downloaded,
                         Qt::UserRole + 13);
    }
    if (m_viewer && m_viewer->hasCurrent()) {
        const Post &cur = m_viewer->currentPost();
        const auto [site, pid] = favIdentity(cur);
        m_viewer->setFaved(m_favDb.isFavourite(site, pid));
    }
    updateViewerBadges();
}

void MainWindow::onMetaMarksChanged(const QString &siteKey, qint64 postId,
                                    bool hasComments, bool hasNotes) {
    if ((!hasComments && !hasNotes) || postId <= 0)
        return;
    // OR-in: parse-time stamps stay authoritative, the fetched tail only
    // adds marks (a post whose search row carried no stamp earns them now).
    auto markVec = [&](QVector<Post> &vec) {
        for (Post &p : vec) {
            if (!p.isRemote || p.id != postId)
                continue;
            const QString key = !p.siteKey.isEmpty()
                                    ? p.siteKey
                                    : RemotePosts::keyForLabel(p.siteName);
            if (key != siteKey)
                continue;
            p.hasComments = p.hasComments || hasComments;
            p.hasNotes = p.hasNotes || hasNotes;
        }
    };
    markVec(m_current);
    markVec(m_browseMerged);
    if (BrowserTab *t = currentTab())
        markVec(t->browseMerged);
    for (int row = 0; row < m_model->rowCount() && row < m_current.size();
         ++row) {
        const Post &p = m_current.at(row);
        if (!p.isRemote || p.id != postId)
            continue;
        const QString key = !p.siteKey.isEmpty()
                                ? p.siteKey
                                : RemotePosts::keyForLabel(p.siteName);
        if (key != siteKey)
            continue;
        m_model->setData(m_model->index(row, 0),
                         p.hasComments || p.commentCount > 0,
                         Qt::UserRole + 14);
        m_model->setData(m_model->index(row, 0), p.hasNotes,
                         Qt::UserRole + 15);
    }
}

void MainWindow::toggleFavourite(const Post &post) {
    if (!m_favDb.isOpen()) {
        statusBar()->showMessage(tr("Favourites unavailable"), 4000);
        return;
    }
    const auto keys =
        favKeysFor(post, favKeysByPath(m_favDb.localPaths()));
    bool faved = false;
    for (const auto &key : keys) {
        if (m_favDb.isFavourite(key.first, key.second)) {
            faved = true;
            break;
        }
    }
    if (faved) {
        for (const auto &key : keys) {
            if (m_favDb.remove(key.first, key.second)) {
                // Explicit removal wins over the automatic coupling: a
                // download finishing later must not resurrect the row
                // (nor the file, when the option deletes it).
                m_unfavVeto.insert(favVetoKey(key.first, key.second));
            }
        }
        if (!post.fileUrl.isEmpty())
            m_downloads->cancelUrls({post.fileUrl});
        m_viewer->setFaved(false);
        updateViewerBadges();
        maybeDeleteUnfavFile(post);
        statusBar()->showMessage(tr("Removed from favourites"), 3000);
    } else {
        if (!m_favDb.add(post)) {
            statusBar()->showMessage(tr("Could not favourite"), 4000);
            return;
        }
        for (const auto &key : keys)
            m_unfavVeto.remove(favVetoKey(key.first, key.second));
        m_viewer->setFaved(true);
        updateViewerBadges();
        statusBar()->showMessage(tr("♥ Favourited"), 3000);
        if (m_settings.favAutoDownload && post.isRemote && !post.fileUrl.isEmpty())
            downloadPosts({post});
    }
    // Refresh every open favourites tab (not just the current one), then
    // repaint the current tab's marks in place (fillModel recomputes them
    // on the next refill anyway).
    refreshFavTabs();
    repaintFavMarks();
}

// Delete the downloaded copy behind an unfavourite when the option is on.
// Only files inside the download folder qualify: library archives are
// never touched by unfavouriting.
void MainWindow::maybeDeleteUnfavFile(const Post &post) {
    if (!m_settings.unfavDeletesFile || post.absFile.isEmpty())
        return;
    const QString dlRoot =
        QDir(effectiveDownloadDir(m_settings)).canonicalPath();
    const QString abs = QFileInfo(post.absFile).canonicalFilePath();
    // Hard safeguard: library archive roots are never written to.
    if (refuseArchiveWrite(abs, dlRoot))
        return;
    if (!dlRoot.isEmpty() && !abs.isEmpty() &&
        (abs == dlRoot || abs.startsWith(dlRoot + u'/'))) {
        if (QFile::remove(abs))
            statusBar()->showMessage(tr("Deleted %1").arg(abs), 6000);
    }
}

