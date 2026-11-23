#include "mainwindow.h"
#include "appinfo.h"
#include "cache.h"
#include "cachejanitor.h"
#include "cachepolicy.h"
#include "cookiegrab.h"
#include "desktopfiles.h"
#include "downloadmanager.h"
#include "filehash.h"
#include "hydrusdialog.h"
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
#include "tempmedia.h"
#include "theme.h"
#include "uicons.h"
#include "tagtypes.h"
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
#include <QMimeData>
#include <QMenuBar>
#include <QMessageBox>
#include <QProcess>
#include <QStatusBar>
#include <QUrl>
#include <memory>

void MainWindow::copyPostFilesToClipboard(const QVector<Post> &posts) {
    if (posts.isEmpty())
        return;
    const auto linked = m_favDb.localPaths();
    QStringList ready;
    QVector<Post> remote;
    for (const Post &p : posts) {
        QString path;
        if (!p.absFile.isEmpty()) {
            const QString c = QFileInfo(p.absFile).canonicalFilePath();
            if (!c.isEmpty() && QFileInfo::exists(c))
                path = c;
        }
        if (path.isEmpty() && p.isRemote) {
            const auto it = linked.find(favIdentity(p));
            if (it != linked.end() && !it.value().isEmpty()) {
                const QString c = QFileInfo(it.value()).canonicalFilePath();
                if (!c.isEmpty() && QFileInfo::exists(c))
                    path = c;
            }
        }
        if (path.isEmpty() && p.isRemote && m_imgCache) {
            const QString url = p.fileUrl.isEmpty() ? p.sampleUrl : p.fileUrl;
            if (RemotePosts::isHttpUrl(url)) {
                const QString cached = m_imgCache->localPath(url, &p);
                if (!cached.isEmpty() && QFileInfo::exists(cached))
                    path = cached;
            }
        }
        if (!path.isEmpty())
            ready << path;
        else if (RemotePosts::isHttpUrl(p.fileUrl))
            remote << p;
        else if (RemotePosts::isHttpUrl(p.sampleUrl))
            remote << p;
    }
    ready.removeDuplicates();
    QPointer<MainWindow> self(this);
    const auto finish = [self](QStringList paths) {
        if (!self)
            return;
        paths.removeDuplicates();
        if (paths.isEmpty()) {
            self->statusBar()->showMessage(
                tr("Could not fetch the media for the clipboard"), 5000);
            return;
        }
        auto *md = new QMimeData();
        QList<QUrl> urls;
        urls.reserve(paths.size());
        for (const QString &p : paths)
            urls << QUrl::fromLocalFile(p);
        md->setUrls(urls);
        QApplication::clipboard()->setMimeData(md);
        self->statusBar()->showMessage(
            tr("Copied %1 file(s) to clipboard").arg(paths.size()), 4000);
    };
    if (remote.isEmpty()) {
        finish(ready);
        return;
    }
    statusBar()->showMessage(tr("Fetching media for clipboard…"), 4000);
    auto remaining = std::make_shared<int>(remote.size());
    auto collected = std::make_shared<QStringList>(ready);
    for (const Post &p : remote) {
        const QString url = p.fileUrl.isEmpty() ? p.sampleUrl : p.fileUrl;
        QString hint = p.ext;
        if (hint.isEmpty()) {
            const QString path = QUrl(url).path();
            const qsizetype dot = path.lastIndexOf(u'.');
            if (dot >= 0 && dot + 1 < path.size())
                hint = path.mid(dot + 1);
        }
        TempMedia::ensureTemp(url, hint, [collected, remaining, finish](
                                              const QString &path) {
            if (!path.isEmpty())
                collected->append(path);
            if (--(*remaining) > 0)
                return;
            finish(*collected);
        });
    }
}

void MainWindow::showGridMenu(const QPoint &pos) {
    if (BrowserTab *ht = currentTab(); ht && ht->isHydrus) {
        showHydrusGridMenu(pos);
        return;
    }
    const QModelIndex clicked = m_grid->indexAt(pos);
    if (!clicked.isValid() || clicked.row() < 0 ||
        clicked.row() >= m_current.size())
        return;
    // Targets: the selection when it contains the click, else the click.
    const QModelIndexList sel = m_grid->selectionModel()->selectedRows();
    bool clickSelected = false;
    for (const QModelIndex &i : sel) {
        if (i.row() == clicked.row()) {
            clickSelected = true;
            break;
        }
    }
    const QModelIndexList rows = clickSelected ? sel : QModelIndexList{clicked};
    QVector<Post> targets;
    for (const QModelIndex &i : rows) {
        if (i.row() >= 0 && i.row() < m_current.size())
            targets.append(m_current.at(i.row()));
    }
    if (targets.isEmpty() || !m_favDb.isOpen())
        return;
    // Path-linked rows for Downloads locals (see favKeysByPath): identity
    // alone misses ("Site", id) rows behind ("local", hash) cards.
    const auto favLocalPaths = m_favDb.localPaths();
    const auto byPath = favKeysByPath(favLocalPaths);
    // On-disk file behind a row: absFile first, otherwise the downloaded
    // copy the favourites DB links for remote rows (a download heals the
    // row, but nothing stamps absFile back on it).
    auto resolvedFile = [&](const Post &p) -> QString {
        if (!p.absFile.isEmpty()) {
            const QString c = QFileInfo(p.absFile).canonicalFilePath();
            if (!c.isEmpty() && QFileInfo::exists(c))
                return c;
        }
        if (p.isRemote) {
            const auto it = favLocalPaths.find(favIdentity(p));
            if (it != favLocalPaths.end() && !it.value().isEmpty()) {
                const QString c = QFileInfo(it.value()).canonicalFilePath();
                if (!c.isEmpty() && QFileInfo::exists(c))
                    return c;
            }
        }
        return {};
    };
    bool allFav = true;
    bool anyRemote = false;
    bool allDownloaded = !targets.isEmpty();
    QStringList anyFiles;
    for (const Post &p : targets) {
        bool marked = false;
        for (const auto &key : favKeysFor(p, byPath)) {
            if (m_favDb.isFavourite(key.first, key.second)) {
                marked = true;
                break;
            }
        }
        if (!marked)
            allFav = false;
        if (p.isRemote && !p.fileUrl.isEmpty())
            anyRemote = true;
        const QString res = resolvedFile(p);
        if (res.isEmpty())
            allDownloaded = false;
        else
            anyFiles << res;
    }
    anyFiles.removeDuplicates();
    QMenu menu(this);
    if (allFav) {
        QAction *unfav = menu.addAction(
            UiIcons::heart(UiIcons::favRed(), false), tr("Unfavourite"));
        connect(unfav, &QAction::triggered, this, [this, targets, byPath] {
            if (targets.size() > 1) {
                QString question = tr("Remove %1 posts from favourites?")
                                       .arg(targets.size());
                if (m_settings.unfavDeletesFile)
                    question += tr("\n\nYour settings also delete their "
                                   "downloaded files.");
                if (QMessageBox::question(this, tr("Mass unfavourite"),
                                          question, QMessageBox::Yes |
                                                       QMessageBox::No,
                                          QMessageBox::No) != QMessageBox::Yes)
                    return;
            }
            QSet<QString> urls;
            int removed = 0;
            // DB rows for path-linked keys (fileUrl for download cancels).
            QHash<QPair<QString, qint64>, Post> favByKey;
            bool needRows = false;
            for (const Post &p : targets) {
                if (favKeysFor(p, byPath).size() > 1) {
                    needRows = true;
                    break;
                }
            }
            if (needRows) {
                for (const Post &p : m_favDb.list())
                    favByKey.insert(favIdentity(p), p);
            }
            for (const Post &p : targets) {
                bool touched = false;
                for (const auto &key : favKeysFor(p, byPath)) {
                    if (m_favDb.remove(key.first, key.second)) {
                        // Explicit removal vetoes any later auto-favourite.
                        m_unfavVeto.insert(
                            favVetoKey(key.first, key.second));
                        touched = true;
                    }
                }
                if (!touched)
                    continue;
                removed++;
                if (!p.fileUrl.isEmpty())
                    urls.insert(p.fileUrl);
                else {
                    // Local card: the row holds the remote URL, if any.
                    for (const auto &key : favKeysFor(p, byPath)) {
                        const Post db = favByKey.value(key);
                        if (!db.fileUrl.isEmpty())
                            urls.insert(db.fileUrl);
                    }
                }
                maybeDeleteUnfavFile(p);
            }
            if (!urls.isEmpty())
                m_downloads->cancelUrls(urls);
            refreshFavTabs();
            repaintFavMarks();
            statusBar()->showMessage(tr("Removed %1 from favourites")
                                         .arg(removed),
                                     4000);
        });
    } else {
        QAction *fav = menu.addAction(UiIcons::heart(UiIcons::favRed(), true),
                                      tr("Favourite"));
        connect(fav, &QAction::triggered, this, [this, targets, byPath] {
            int added = 0;
            for (const Post &p : targets) {
                bool already = false;
                for (const auto &key : favKeysFor(p, byPath)) {
                    if (m_favDb.isFavourite(key.first, key.second)) {
                        already = true;
                        break;
                    }
                }
                if (already)
                    continue; // path-linked row exists: no duplicate
                if (m_favDb.add(p)) {
                    for (const auto &key : favKeysFor(p, byPath))
                        m_unfavVeto.remove(
                            favVetoKey(key.first, key.second));
                    added++;
                }
            }
            refreshFavTabs();
            repaintFavMarks();
            statusBar()->showMessage(
                tr("Favourited %1").arg(added), 4000);
        });
    }
    QAction *dl = menu.addAction(UiIcons::icon(QStringLiteral("download")),
                                 tr("Download"));
    dl->setEnabled(anyRemote && !allDownloaded); // downloaded rows delete instead
    connect(dl, &QAction::triggered, this, [this, targets] {
        QVector<Post> remote;
        for (const Post &p : targets) {
            if (p.isRemote && !p.fileUrl.isEmpty())
                remote.append(p);
        }
        // Explicit download lifts the veto: the user asked for the file.
        for (const Post &p : remote) {
            const auto [site, pid] = favIdentity(p);
            m_unfavVeto.remove(favVetoKey(site, pid));
        }
        downloadPosts(remote);
    });
    // Clipboard actions: post page and media URL/path, for stills, GIFs
    // and videos alike (multi-selection joins with newlines).
    if (!targets.isEmpty()) {
        QStringList postLinks, mediaLinks, localPaths;
        for (const Post &p : targets) {
            if (!p.postUrl.isEmpty() && RemotePosts::isHttpUrl(p.postUrl))
                postLinks << p.postUrl;
            if (!p.fileUrl.isEmpty() && RemotePosts::isHttpUrl(p.fileUrl))
                mediaLinks << p.fileUrl;
            else if (!p.absFile.isEmpty())
                mediaLinks << p.absFile;
            const QString res = resolvedFile(p);
            if (!res.isEmpty())
                localPaths << res;
        }
        if (!postLinks.isEmpty()) {
            QAction *a = menu.addAction(
                targets.size() == 1 ? tr("Copy post link")
                                    : tr("Copy post links"));
            connect(a, &QAction::triggered, this, [postLinks] {
                QApplication::clipboard()->setText(
                    postLinks.join(u'\n'));
            });
        }
        if (!mediaLinks.isEmpty()) {
            QAction *a = menu.addAction(
                targets.size() == 1 ? tr("Copy media link")
                                    : tr("Copy media links"));
            connect(a, &QAction::triggered, this, [mediaLinks] {
                QApplication::clipboard()->setText(
                    mediaLinks.join(u'\n'));
            });
        }
        if (!localPaths.isEmpty()) {
            QAction *a = menu.addAction(tr("Copy file path"));
            connect(a, &QAction::triggered, this, [localPaths] {
                QApplication::clipboard()->setText(
                    localPaths.join(u'\n'));
            });
        }
        // Favourite-only/remote cards keep the downloaded-card clipboard
        // action: missing bytes are pulled into the temp folder first.
        bool anyMedia = false;
        for (const Post &p : targets) {
            const QString res = resolvedFile(p);
            if (!res.isEmpty() || RemotePosts::isHttpUrl(p.fileUrl) ||
                RemotePosts::isHttpUrl(p.sampleUrl)) {
                anyMedia = true;
                break;
            }
        }
        if (anyMedia) {
            QAction *a = menu.addAction(
                targets.size() == 1 ? tr("Copy file (clipboard)")
                                    : tr("Copy files (clipboard)"));
            connect(a, &QAction::triggered, this, [this, targets] {
                copyPostFilesToClipboard(targets);
            });
        }
    }
    if (targets.size() == 1) {
        const Post one = targets.first();
        QAction *meta = menu.addAction(tr("View metadata…"));
        connect(meta, &QAction::triggered, this,
                [this, one] { showPostMetadata(one); });
        const QString res = resolvedFile(one);
        const bool hasMd5 = isMd5Hex(one.md5);
        if (m_settings.hydrusEnabled && (!res.isEmpty() || hasMd5)) {
            QAction *hydrus = menu.addAction(tr("Hydrus tags…"));
            hydrus->setToolTip(
                tr("Read-only: look this file up in Hydrus by content hash"));
            connect(hydrus, &QAction::triggered, this,
                    [this, one] { showHydrusTags(one); });
        }
        if (!res.isEmpty()) {
            QAction *show = menu.addAction(tr("Show in folder"));
            connect(show, &QAction::triggered, this, [res] {
                DesktopFiles::showInFolder(res);
            });
        }
    }
    // Reverse image search (engines need a fetchable remote file; local
    // cards have none, so the submenu stays disabled without one).
    QMenu *revMenu = menu.addMenu(tr("Reverse image search"));
    bool anyHttp = false;
    for (const Post &p : targets) {
        if (p.isRemote && RemotePosts::isHttpUrl(p.fileUrl)) {
            anyHttp = true;
            break;
        }
    }
    revMenu->setEnabled(anyHttp);
    if (anyHttp) {
        for (const Post &p : targets) {
            if (!(p.isRemote && RemotePosts::isHttpUrl(p.fileUrl)))
                continue;
            const auto engines = RemotePosts::reverseSearchUrls(p.fileUrl);
            for (const auto &[label, url] : engines) {
                QAction *a = revMenu->addAction(
                    targets.size() == 1
                        ? label
                        : tr("%1 (#%2)").arg(label).arg(p.id));
                connect(a, &QAction::triggered, this, [url] {
                    QDesktopServices::openUrl(QUrl(url));
                });
            }
            if (targets.size() > 1)
                revMenu->addSeparator();
        }
    }
    // Tag editing is a stray-only affair: every selected target must be a
    // stray file; the dialogs prefill from their sidecars. Bulk (2+)
    // opens the mass editor (one window, per-bucket, paste + steal via
    // URL); a single stray keeps the one-box editor.
    bool allStray = !targets.isEmpty();
    for (const Post &t : targets)
        if (!Stray::isStrayPost(t)) { allStray = false; break; }
    if (allStray) {
        QAction *edit = menu.addAction(targets.size() > 1
                                          ? tr("Edit tags… (%1 strays)").arg(targets.size())
                                          : tr("Edit tags…"));
        connect(edit, &QAction::triggered, this, [this, targets] {
            if (targets.size() == 1) {
                editStrayTags(targets.first());
                return;
            }
            StrayMassTagDialog dlg(targets, hydrusConfigFromSettings(m_settings),
                                   this);
            if (dlg.exec() == QDialog::Accepted)
                applyStrayTagEdits(dlg.edits());
        });
    }
    // Pictures (still or animated) can become the home mascot -
    // including downloaded remote rows, whose copy the favourites DB
    // links (absFile is empty on those). Videos are never offered.
    if (targets.size() == 1) {
        Post mascotPost = targets.first();
        const QString res = resolvedFile(mascotPost);
        if (!res.isEmpty() && WelcomePage::isMascotFile(res)) {
            mascotPost.absFile = res;
            QAction *mascot = menu.addAction(tr("Set as mascot"));
            connect(mascot, &QAction::triggered, this,
                    [this, mascotPost] { setAsMascot(mascotPost); });
        }
    }
    // Delete is a disk-only affair unless "deleting unfavourites" is on:
    // it removes the files (plus our download sidecars) and leaves the
    // favourites alone. Unfavourite above is the mirror: it drops the DB
    // entries and only deletes files when "unfavouriting deletes" is on.
    // Remote-only posts have no file, so they stay Delete-disabled.
    // Local archives are read-only: no Delete there at all (the downloads
    // folder browsed as a tab still allows it).
    bool archiveTab = false;
    if (BrowserTab *t = currentTab();
        t && !t->isRemote && !t->isFavourites) {
        const QString dlRoot =
            QDir(effectiveDownloadDir(m_settings)).canonicalPath();
        const QString tabRoot = QDir(t->root).canonicalPath();
        archiveTab =
            tabRoot.isEmpty() || dlRoot.isEmpty() || tabRoot != dlRoot;
    }
    QAction *del = menu.addAction(tr("Delete from disk…"));
    del->setEnabled(!anyFiles.isEmpty() && !archiveTab);
    connect(del, &QAction::triggered, this,
            [this, targets, anyFiles, resolvedFile] {
        if (anyFiles.isEmpty())
            return;
        const bool unfav = m_settings.deleteUnfavs;
        // Regenerability per file: a kept favourite with a source link
        // comes back via re-download; anything else is gone for good.
        auto sidecarUrl = [](const QString &absFile) {
            const QFileInfo fi(absFile);
            const QString dir = fi.absolutePath();
            const QString stem = fi.completeBaseName();
            for (const QString &sp :
                 {dir + u'/' + stem + QStringLiteral(".json"),
                  absFile + QStringLiteral(".json")}) {
                QFile f(sp);
                if (!f.open(QIODevice::ReadOnly))
                    continue;
                QJsonParseError err;
                const QJsonDocument doc =
                    QJsonDocument::fromJson(f.readAll(), &err);
                if (err.error == QJsonParseError::NoError && doc.isObject()) {
                    const QString u = doc.object()
                                          .value(QStringLiteral("file_url"))
                                          .toString();
                    if (!u.isEmpty())
                        return u;
                }
            }
            return QString();
        };
        // Kept favourites losing their file are still regenerated from
        // the DB row's remote URLs (and the sidecar file is removed;
        // a lone sidecar without media can never be re-scanned, so
        // keeping it only littered). Doomed (no source link) is only
        // for the warning below.
        QSet<QString> doomed;
        // Path -> fav keys pointing at it (recorded before deletion,
        // while canonicalization still resolves). Local scanned posts
        // carry a ("local", file-hash) identity that never matches
        // their ("Site", id) favourite rows, so identity alone misses
        // Downloads-tab deletes and the startup offer comes back.
        const QHash<QPair<QString, qint64>, QString> recordedPaths =
            m_favDb.localPaths();
        const auto byCanonPath = favKeysByPath(recordedPaths);
        QHash<QPair<QString, qint64>, Post> favByKey;
        for (const Post &p : m_favDb.list())
            favByKey.insert(favIdentity(p), p);
        QSet<QString> warned;
        for (const Post &p : targets) {
            const QString abs = resolvedFile(p);
            if (abs.isEmpty() || !anyFiles.contains(abs))
                continue;
            const auto [site, pid] = favIdentity(p);
            if (!m_favDb.isFavourite(site, pid) || unfav)
                continue; // gone from favourites too: nothing to warn about
            warned.insert(favVetoKey(site, pid));
            const bool regen =
                p.isRemote ? !p.fileUrl.isEmpty() : !sidecarUrl(abs).isEmpty();
            if (!regen)
                doomed.insert(abs);
        }
        // Path-linked favourites missed above (Downloads-tab locals):
        // warn by the DB row's own source link.
        for (const QString &abs : anyFiles) {
            for (const auto &key : byCanonPath.value(abs)) {
                const QString veto = favVetoKey(key.first, key.second);
                if (warned.contains(veto) ||
                    !m_favDb.isFavourite(key.first, key.second) || unfav)
                    continue;
                warned.insert(veto);
                const Post db = favByKey.value(key);
                const bool regen =
                    (!db.fileUrl.isEmpty() &&
                     RemotePosts::isHttpUrl(db.fileUrl)) ||
                    !sidecarUrl(abs).isEmpty();
                if (!regen)
                    doomed.insert(abs);
            }
        }
        QString question =
            unfav ? tr("Permanently delete %1 file(s) from disk and remove "
                       "them from favourites?")
                        .arg(anyFiles.size())
                  : tr("Permanently delete %1 file(s) from disk?")
                        .arg(anyFiles.size());
        if (!doomed.isEmpty())
            question += tr("\n\n%1 favourited file(s) have no source link "
                           "and cannot be re-downloaded.")
                            .arg(doomed.size());
        if (QMessageBox::question(this, tr("Delete"), question,
                                  QMessageBox::Yes | QMessageBox::No,
                                  QMessageBox::No) != QMessageBox::Yes)
            return;
        const QString dlRoot =
            QDir(effectiveDownloadDir(m_settings)).canonicalPath();
        int gone = 0;
        for (const QString &f : anyFiles) {
            // Hard safeguard: library archive roots are never written to.
            if (refuseArchiveWrite(f, dlRoot))
                continue;
            if (QFile::remove(f)) {
                gone++;
                // Our sidecars share the stem (<stem>.json) or ride flat
                // (<media>.json): remove both alongside the media so no
                // orphan is left behind. Archive sidecars (outside the
                // download root) are the user's metadata and stay.
                if (!dlRoot.isEmpty() &&
                    (f == dlRoot || f.startsWith(dlRoot + u'/'))) {
                    for (const QString &sp :
                         DownloadManager::sidecarPathsFor(f))
                        QFile::remove(sp);
                }
            }
        }
        if (gone > 0)
            invalidateDownloadIndex();
        int unfaved = 0;
        if (m_settings.deleteUnfavs) {
            QSet<QString> urls;
            QSet<QString> removed;
            for (const Post &p : targets) {
                const auto [site, pid] = favIdentity(p);
                if (m_favDb.remove(site, pid)) {
                    removed.insert(favVetoKey(site, pid));
                    m_unfavVeto.insert(favVetoKey(site, pid));
                    if (!p.fileUrl.isEmpty())
                        urls.insert(p.fileUrl);
                    unfaved++;
                }
            }
            // Path-linked rows missed by identity (Downloads-tab locals).
            for (const QString &abs : anyFiles) {
                for (const auto &key : byCanonPath.value(abs)) {
                    const QString veto = favVetoKey(key.first, key.second);
                    if (removed.contains(veto))
                        continue;
                    if (m_favDb.remove(key.first, key.second)) {
                        removed.insert(veto);
                        m_unfavVeto.insert(veto);
                        const Post db = favByKey.value(key);
                        if (!db.fileUrl.isEmpty())
                            urls.insert(db.fileUrl);
                        unfaved++;
                    }
                }
            }
            if (!urls.isEmpty())
                m_downloads->cancelUrls(urls);
        } else {
            // A kept favourite whose file is gone and which has no
            // remote media or post link left can never render again:
            // leaving it behind is a permanent empty card, so drop it
            // (reported below like any other favourite removal).
            //
            // Every other kept favourite loses its download link here:
            // files deleted through the GUI must not come back via the
            // startup missing-download offer (that offer is only for
            // files deleted behind the app's back). The favourite
            // itself stays and falls back to remote.
            QSet<QString> handled;
            for (const Post &p : targets) {
                if (p.absFile.isEmpty() || QFileInfo::exists(p.absFile))
                    continue;
                const auto [site, pid] = favIdentity(p);
                handled.insert(favVetoKey(site, pid));
                if (!m_favDb.isFavourite(site, pid))
                    continue;
                const bool dead =
                    p.fileUrl.isEmpty() && p.sampleUrl.isEmpty() &&
                    p.previewUrl.isEmpty() &&
                    !p.postUrl.startsWith(QStringLiteral("http://")) &&
                    !p.postUrl.startsWith(QStringLiteral("https://"));
                if (dead) {
                    // Viewer refresh can't heal these either.
                    if (m_favDb.remove(site, pid)) {
                        m_unfavVeto.insert(favVetoKey(site, pid));
                        unfaved++;
                    }
                } else {
                    m_favDb.setLocalPath(site, pid, QString());
                }
            }
            // Path-linked favourites missed by identity
            // (Downloads-tab locals): forget the download link by path
            // so the startup offer stays silent for GUI deletes.
            for (const QString &abs : anyFiles) {
                for (const auto &key : byCanonPath.value(abs)) {
                    const QString veto = favVetoKey(key.first, key.second);
                    if (handled.contains(veto))
                        continue;
                    handled.insert(veto);
                    if (!m_favDb.isFavourite(key.first, key.second))
                        continue;
                    const Post db = favByKey.value(key);
                    const bool dead =
                        db.fileUrl.isEmpty() && db.sampleUrl.isEmpty() &&
                        db.previewUrl.isEmpty() &&
                        !db.postUrl.startsWith(QStringLiteral("http://")) &&
                        !db.postUrl.startsWith(QStringLiteral("https://"));
                    if (dead) {
                        if (m_favDb.remove(key.first, key.second)) {
                            m_unfavVeto.insert(veto);
                            unfaved++;
                        }
                    } else {
                        m_favDb.setLocalPath(key.first, key.second, QString());
                    }
                }
            }
        }
        // Other local tabs showing these folders are stale now: flag
        // them so landing on one rescans (mirrors the download path).
        for (const QString &f : anyFiles) {
            const QString parent = QFileInfo(f).absolutePath();
            const QString canon = QDir(parent).canonicalPath();
            m_dirtyRoots.insert(canon.isEmpty() ? parent : canon);
        }
        refreshFavTabs();
        if (m_curTab >= 0 && !currentTab()->isRemote &&
            !currentTab()->isFavourites)
            startScan(m_root); // archive changed: rescan
        statusBar()->showMessage(
            unfaved > 0
                ? tr("Deleted %1 file(s), removed %2 from favourites")
                      .arg(gone)
                      .arg(unfaved)
                : tr("Deleted %1 file(s)").arg(gone),
            6000);
    });
    menu.exec(m_grid->viewport()->mapToGlobal(pos));
}

