#include "mainwindow.h"
#include "appinfo.h"
#include "cache.h"
#include "cachejanitor.h"
#include "cachepolicy.h"
#include "cookiegrab.h"
#include "downloadmanager.h"
#include "filehash.h"
#include "gallerydl.h"
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

void MainWindow::downloadPosts(const QVector<Post> &posts, bool confirmBulk) {
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
    // A remote batch is an explicit network action: open the gate and
    // build the browser fallback only when there is something to fetch.
    allowNetwork();
    ensureWebBackend();
    const QString dest = effectiveDownloadDir(m_settings);
    const QList<DownloadJob> jobs = DownloadManager::plan(remote, dest);
    if (jobs.isEmpty()) {
        statusBar()->showMessage(tr("Nothing to download"), 4000);
        return;
    }
    // A multi-post batch is easy to trigger by accident (Ctrl+D, a
    // selection, a stray link list) and can be large: name the count and
    // destination before starting. Single posts stay one-click.
    if (confirmBulk && jobs.size() > 1) {
        const QString question =
            tr("Download %1 posts to:\n%2\n\nStart the batch?")
                .arg(jobs.size())
                .arg(QDir::toNativeSeparators(dest));
        if (QMessageBox::question(this, tr("Mass download"), question,
                                  QMessageBox::Yes | QMessageBox::No,
                                  QMessageBox::No) != QMessageBox::Yes)
            return;
    }
    m_downloads->enqueue(jobs); // no popup: progress lives in the feedbar
}

// Gallery links (Pixiv/Fanbox/Pawchive/…) via the system gallery-dl: it
// owns extractors + login (transport only, no metadata), we own the
// destination (_Stray), the progress view and the stray sidecars the
// flight writes for new files (tags empty for the user, post_url/source
// = pasted page). One flight per pasted URL so every file attributes to
// the page it came from. No auto-favourite: a gallery flight can land
// hundreds of files and Favourites is not a bulk inbox (single-file
// strays keep their toggle). Rescans the current tab when it sits
// under dest.
void MainWindow::runGalleryDl(const QStringList &urls) {
    if (urls.isEmpty())
        return;
    const QString binary = GalleryDl::findBinary();
    if (binary.isEmpty()) {
        statusBar()->showMessage(
            tr("gallery-dl not found (install the system package for "
               "gallery links)"),
            8000);
        return;
    }
    const QString dest =
        effectiveDownloadDir(m_settings) + u'/' + Stray::dirName();
    auto chain = QSharedPointer<GalleryChain>::create();
    chain->binary = binary;
    chain->dest = dest;
    chain->all = urls;
    chain->pending = urls;
    auto *dlg = new GalleryDlDialog(dest, this);
    chain->dlg = dlg;
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    connect(dlg, &GalleryDlDialog::cancelRequested, this,
            [chain] {
                chain->cancelled = true;
                chain->pending.clear();
                if (chain->runner)
                    chain->runner->cancel();
            });
    connect(dlg, &GalleryDlDialog::browseRequested, this,
            &MainWindow::openPath);
    dlg->show();
    dlg->raise();
    startGalleryFlight(chain);
}

// One flight of a gallery-dl batch (runner parented to the dialog: an
// X-close mid-flight kills it, and the finished handler either starts
// the next URL or reports the batch and lets the chain drop).
void MainWindow::startGalleryFlight(QSharedPointer<GalleryChain> chain) {
    GalleryDlDialog *dlg = chain->dlg;
    if (chain->cancelled || chain->pending.isEmpty() || !dlg)
        return;
    const QString url = chain->pending.takeFirst();
    if (!chain->pending.isEmpty())
        dlg->showCurrent(tr("URL %1 of %2: %3")
                             .arg(chain->all.size() - chain->pending.size())
                             .arg(chain->all.size())
                             .arg(url));
    auto *runner = new GalleryDlRunner(chain->binary, chain->dest, url, dlg);
    chain->runner = runner;
    connect(runner, &GalleryDlRunner::logLine, dlg,
            &GalleryDlDialog::appendLog);
    connect(runner, &GalleryDlRunner::progress, dlg,
            &GalleryDlDialog::showCurrent);
    connect(runner, &GalleryDlRunner::finished, this,
            [this, chain](const GalleryDlRunner::Summary &s) {
                chain->runner = nullptr;
                chain->files += s.filesFound;
                chain->sidecars += s.sidecarsWritten;
                if (!s.ok && !chain->cancelled)
                    chain->errors << tr("%1: %2").arg(s.url, s.error);
                GalleryDlDialog *dlg = chain->dlg;
                if (!chain->pending.isEmpty() && !chain->cancelled) {
                    startGalleryFlight(chain);
                    return;
                }
                // Last flight: report the batch.
                const bool ok = chain->errors.isEmpty();
                const QString err = chain->errors.join(u'\n');
                if (dlg)
                    dlg->showFinished(ok, chain->files, err);
                invalidateDownloadIndex(); // new bytes = download badges
                if (chain->cancelled) {
                    statusBar()->showMessage(tr("gallery-dl cancelled"),
                                             6000);
                } else if (ok) {
                    if (chain->files > 0) {
                        statusBar()->showMessage(
                            tr("gallery-dl: %1 new file(s) in _Stray, "
                               "%2 sidecar(s)")
                                .arg(chain->files)
                                .arg(chain->sidecars),
                            8000);
                        BrowserTab *t = currentTab();
                        if (t && !t->isRemote && !t->isFavourites &&
                            !t->isHydrus &&
                            (t->root == chain->dest ||
                             t->root.startsWith(chain->dest + u'/')))
                            startScan(m_root);
                    } else {
                        statusBar()->showMessage(
                            tr("gallery-dl finished: nothing new"), 6000);
                    }
                } else {
                    statusBar()->showMessage(
                        tr("gallery-dl failed: %1").arg(err), 12000);
                }
            });
    runner->start();
}

void MainWindow::showDlDialog() {
    const QString dest = effectiveDownloadDir(m_settings);
    if (!m_dlDialog) {
        m_dlDialog = new DownloadsDialog(m_downloads, dest, this);
        m_dlDialog->setAttribute(Qt::WA_DeleteOnClose, false);
        connect(m_downloads, &DownloadManager::batchStarted, m_dlDialog,
                &DownloadsDialog::onBatchStarted);
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
    // Recompute the current tab's favourite/download marks in place (bulk
    // fav/unfav must not wait for the next refill) and refresh the viewer
    // badges + fav
    // toggle for whatever post is on screen.
    if (!m_favDb.isOpen() || !m_model)
        return;
    if (isHydrusTab())
        return; // no favourite/download marks in the Hydrus tab
    const QHash<QPair<QString, qint64>, QString> favLocal =
        m_favDb.localPaths();
    const auto byPath = favKeysByPath(favLocal);
    for (int row = 0; row < m_model->rowCount() && row < m_current.size();
         ++row) {
        Post &p = m_current[row];
        p.downloaded = isPostDownloaded(p);
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
    if (isHydrusTab()) {
        statusBar()->showMessage(
            tr("Hydrus media is always local; favourites do not apply"),
            4000);
        return;
    }
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
        statusBar()->showMessage(tr("Favourited"), 3000);
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
        if (QFile::remove(abs)) {
            invalidateDownloadIndex();
            statusBar()->showMessage(tr("Deleted %1").arg(abs), 6000);
        }
    }
}

