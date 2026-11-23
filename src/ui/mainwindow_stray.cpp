#include "mainwindow.h"
#include "appinfo.h"
#include "cache.h"
#include "cachejanitor.h"
#include "cachepolicy.h"
#include "cookiegrab.h"
#include "downloadmanager.h"
#include "filehash.h"
#include "gallerydl.h"
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

void MainWindow::openStrayDialog() {
    auto *dlg = new StrayDialog(m_settings.strayAutoFav, this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    connect(dlg, &StrayDialog::autoFavChanged, this, [this](bool on) {
        m_settings.strayAutoFav = on;
        saveSettings(m_settings);
    });
    // Shared link resolution: booru post pages become tagged posts through
    // their public API (no entry is added to the booru list), direct media
    // URLs pass through unchanged. Failures are reported once, after the
    // whole batch ran.
    auto resolveLinks =
        [this](const QStringList &urls,
               const std::function<void(const QVector<Post> &)> &deliver) {
            auto *resolver = new StrayResolver(this);
            connect(resolver, &StrayResolver::progress, this,
                    [this](int done, int total, const QString &) {
                        statusBar()->showMessage(
                            tr("Reading link %1 of %2…")
                                .arg(done + 1)
                                .arg(total),
                            4000);
                    });
            connect(resolver, &StrayResolver::finished, this,
                    [this, resolver,
                     deliver](const QVector<StrayResolver::Result> &results) {
                        resolver->deleteLater();
                        QVector<Post> posts;
                        QStringList errors;
                        for (const auto &r : results) {
                            if (r.ok)
                                posts.append(r.post);
                            else
                                errors << tr("%1: %2").arg(r.sourceUrl, r.error);
                        }
                        if (!errors.isEmpty()) {
                            statusBar()->showMessage(
                                tr("Skipped %1 link(s): %2")
                                    .arg(errors.size())
                                    .arg(errors.first()),
                                12000);
                            if (posts.isEmpty())
                                QMessageBox::warning(this, tr("Stray links"),
                                                     errors.join(u'\n'));
                        }
                        deliver(posts);
                    });
            resolver->resolve(urls);
        };
    connect(dlg, &StrayDialog::downloadRequested, this,
            [this, resolveLinks](const QStringList &urls) {
                // Gallery pages (Pixiv/Fanbox/Pawchive/…) go to the system
                // gallery-dl first (its login + extractors); everything
                // else keeps the built-in resolver (direct media as-is,
                // booru post pages with bucketed sidecars). Without a
                // system binary, gallery links fall back to the resolver
                // too (best effort: booru API/HTML, else an error row).
                QStringList gallery, simple;
                if (GalleryDl::available()) {
                    for (const QString &u : urls) {
                        if (GalleryDl::handlesUrl(u))
                            gallery << u;
                        else
                            simple << u;
                    }
                } else {
                    simple = urls;
                    bool anyGallery = false;
                    for (const QString &u : urls) {
                        if (GalleryDl::handlesUrl(u)) {
                            anyGallery = true;
                            break;
                        }
                    }
                    if (anyGallery)
                        statusBar()->showMessage(
                            tr("gallery-dl not found: gallery links fall back "
                               "to the built-in resolver (install gallery-dl "
                               "for full support)"),
                            12000);
                }
                if (!gallery.isEmpty())
                    runGalleryDl(gallery);
                if (!simple.isEmpty())
                    resolveLinks(simple, [this](const QVector<Post> &posts) {
                        if (!posts.isEmpty())
                            downloadPosts(posts);
                    });
            });
    connect(dlg, &StrayDialog::favouriteRequested, this,
            [this, resolveLinks](const QStringList &urls) {
                resolveLinks(urls, [this](const QVector<Post> &posts) {
                    if (posts.isEmpty())
                        return;
                    if (!m_favDb.isOpen()) {
                        statusBar()->showMessage(
                            tr("Favourites are unavailable"), 6000);
                        return;
                    }
                    QVector<Post> favs;
                    for (const Post &p : posts) {
                        const auto [site, pid] = favIdentity(p);
                        if (m_unfavVeto.contains(favVetoKey(site, pid)))
                            continue;
                        favs.append(p);
                    }
                    const int added = m_favDb.addMany(favs);
                    if (added > 0) {
                        refreshFavTabs();
                        repaintFavMarks();
                        updateViewerBadges();
                        statusBar()->showMessage(
                            tr("Favourited %1 link(s) without downloading")
                                .arg(added),
                            6000);
                    } else {
                        statusBar()->showMessage(
                            tr("Nothing new to favourite"), 6000);
                    }
                });
            });
    connect(dlg, &StrayDialog::importRequested, this,
            &MainWindow::importStrayFiles);
    connect(dlg, &QDialog::finished, this, [this] {
        refreshFavTabs();
        invalidateDownloadIndex();
        repaintFavMarks();
    });
    dlg->show();
    dlg->raise();
}

void MainWindow::importStrayFiles(const QStringList &paths) {
    const QString dest = effectiveDownloadDir(m_settings) + u'/' +
                         Stray::dirName();
    QDir().mkpath(dest);
    // Hash + copy off the UI thread; big videos must not freeze it.
    // Result: (failed sources, imported absolute paths).
    auto *watcher =
        new QFutureWatcher<QPair<QStringList, QStringList>>(this);
    connect(watcher, &QFutureWatcher<QPair<QStringList, QStringList>>::finished,
            this, [this, watcher, dest] {
                const auto [errors, imported] = watcher->result();
                watcher->deleteLater();
                const int ok = static_cast<int>(imported.size());
                int faved = 0;
                if (ok > 0 && m_settings.strayAutoFav && m_favDb.isOpen()) {
                    QVector<Post> favs;
                    favs.reserve(ok);
                    for (const QString &abs : imported) {
                        Post p;
                        p.absFile = abs;
                        p.file = abs;
                        p.ext = QFileInfo(abs).suffix().toLower();
                        // Sidecar holds the hand-written tags (empty until
                        // the user edits them); snapshot them into the row.
                        QFile jf(QFileInfo(abs).absolutePath() + u'/' +
                                 QFileInfo(abs).completeBaseName() +
                                 QStringLiteral(".json"));
                        if (jf.open(QIODevice::ReadOnly)) {
                            QJsonParseError err;
                            const QJsonDocument doc = QJsonDocument::fromJson(
                                jf.readAll(), &err);
                            if (err.error == QJsonParseError::NoError &&
                                doc.isObject())
                                MetaParser::parse(doc.object(), p);
                            p.absFile = abs;
                            p.file = abs;
                        }
                        favs.append(p);
                    }
                    faved = m_favDb.addMany(favs);
                    if (faved > 0)
                        refreshFavTabs();
                }
                if (!errors.isEmpty()) {
                    statusBar()->showMessage(
                        tr("Imported %1 file(s), %2 failed").arg(ok).arg(
                            errors.size()),
                        8000);
                } else if (faved > 0) {
                    statusBar()->showMessage(
                        tr("Imported %1 file(s) to _Stray, favourited %2")
                            .arg(ok)
                            .arg(faved),
                        6000);
                } else {
                    statusBar()->showMessage(
                        tr("Imported %1 file(s) to _Stray").arg(ok), 6000);
                }
                BrowserTab *t = currentTab();
                if (t && !t->isRemote && !t->isFavourites &&
                    (t->root == dest ||
                     t->root.startsWith(dest + u'/')))
                    startScan(m_root);
            });
    watcher->setFuture(QtConcurrent::run([paths, dest] {
        QStringList errors;
        QStringList imported;
        for (const QString &src : paths) {
            const QFileInfo fi(src);
            if (!fi.isFile() || !fi.isReadable()) {
                errors << src;
                continue;
            }
            const QString md5 = fileContentMd5(src);
            QString ext = fi.suffix().toLower();
            if (ext.isEmpty())
                ext = QStringLiteral("bin");
            const QString name = Stray::dirName() + u'_' + md5 + u'.' + ext;
            if (md5.isEmpty() || QFile::exists(dest + u'/' + name)) {
                if (md5.isEmpty())
                    errors << src;
                continue; // already imported (or unreadable)
            }
            if (!QFile::copy(src, dest + u'/' + name)) {
                errors << src;
                continue;
            }
            QJsonObject sidecar;
            sidecar[QStringLiteral("tags")] = QString();
            sidecar[QStringLiteral("file_ext")] = ext;
            sidecar[QStringLiteral("md5")] = md5;
            sidecar[QStringLiteral("stray")] = true;
            const QString stem = name.left(name.lastIndexOf(u'.'));
            QFile jf(dest + u'/' + stem + QStringLiteral(".json"));
            if (jf.open(QIODevice::WriteOnly | QIODevice::Truncate))
                jf.write(QJsonDocument(sidecar).toJson(
                    QJsonDocument::Indented));
            imported << dest + u'/' + name;
        }
        return qMakePair(errors, imported);
    }));
}

void MainWindow::editStrayTags(const Post &post) {
    const QFileInfo fi(post.absFile);
    const QString sidecar = fi.absolutePath() + u'/' +
                            fi.completeBaseName() +
                            QStringLiteral(".json");
    QString tags;
    QJsonObject obj;
    {
        QFile f(sidecar);
        if (f.open(QIODevice::ReadOnly)) {
            QJsonParseError err;
            const QJsonDocument doc =
                QJsonDocument::fromJson(f.readAll(), &err);
            if (err.error == QJsonParseError::NoError && doc.isObject())
                obj = doc.object();
            tags = obj.value(QStringLiteral("tags")).toString();
        }
    }
    if (tags.isEmpty())
        tags = post.tags;
    StrayTagEdit existing;
    existing.absFile = post.absFile;
    existing.fileName = fi.fileName();
    auto bucket = [&obj](const char *key, const QString &fallback) {
        const QString value =
            obj.value(QString::fromLatin1(key)).toString();
        return value.isEmpty() ? fallback : value;
    };
    existing.artist = bucket("tags_artist", post.tagsArtist);
    existing.character = bucket("tags_character", post.tagsCharacter);
    existing.copyright = bucket("tags_copyright", post.tagsCopyright);
    existing.general = bucket("tags_general", post.tagsGeneral);
    existing.meta = bucket("tags_meta", post.tagsMeta);
    StrayTagDialog dlg(fi.fileName(), tags,
                       hydrusConfigFromSettings(m_settings), existing, this);
    if (dlg.exec() != QDialog::Accepted)
        return;
    // A Hydrus fetch knows its namespaces (artist/character/series/meta),
    // so write those buckets directly; manual additions after the fetch
    // land in General. Without a fetch the old flat/auto-classify path
    // stays in charge.
    if (dlg.hasHydrusTags()) {
        applyStrayTagEdits({dlg.hydrusTagSplit()});
        return;
    }
    // Normalize like the rest of the tag universe: all whitespace runs
    // become single spaces (Tab/Enter must never glue itself to a tag),
    // lowercase, deduped. (The classifier, search and sidebar all key on
    // lowercase.)
    QStringList kept;
    for (const QString &t :
         dlg.tags().simplified().toLower().split(QChar::Space,
                                                  Qt::SkipEmptyParts)) {
        if (!kept.contains(t))
            kept << t;
    }
    obj[QStringLiteral("tags")] = kept.join(u' ');
    obj[QStringLiteral("stray")] = true;
    // The hand edit is the new truth: stale API buckets must not keep
    // removed tags alive. Without them, parseStray treats the flat blob as
    // untyped and the normal classifier re-buckets it after the save.
    for (const char *k : {"tags_artist", "tags_character", "tags_copyright",
                          "tags_general", "tags_meta", "stray_tags"})
        obj.remove(QString::fromLatin1(k));
    // Hard safeguard: library archive roots are never written to.
    if (refuseArchiveWrite(sidecar, effectiveDownloadDir(m_settings))) {
        statusBar()->showMessage(
            tr("Library archives are read-only"), 6000);
        return;
    }
    {
        QFile f(sidecar);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            statusBar()->showMessage(
                tr("Cannot write %1").arg(sidecar), 6000);
            return;
        }
        f.write(
            QJsonDocument(obj).toJson(QJsonDocument::Indented));
    }
    // The scan cache keys on the MEDIA file (mtime+size): a sidecar-only
    // edit would read stale, so the tags never became searchable. Touch
    // the media so future rescans reparse the sidecar too.
    if (!refuseArchiveWrite(post.absFile, effectiveDownloadDir(m_settings))) {
        QFile touch(post.absFile);
        if (touch.open(QIODevice::ReadWrite))
            touch.setFileTime(QDateTime::currentDateTime(),
                              QFileDevice::FileModificationTime);
    }
    applyStrayTagUpdates({{post.absFile, obj}});
    statusBar()->showMessage(tr("Tags saved"), 4000);
}

void MainWindow::showHydrusTags(const Post &post) {
    if (!m_settings.hydrusEnabled)
        return;
    QString path = post.absFile;
    if (!path.isEmpty() && !QFileInfo::exists(path))
        path.clear();
    if (path.isEmpty() && m_favDb.isOpen()) {
        const QString linked = m_favDb.localPaths().value(favIdentity(post));
        if (!linked.isEmpty() && QFileInfo::exists(linked))
            path = linked;
    }
    const QString md5 = isMd5Hex(post.md5) ? post.md5.toLower() : QString();
    QString label = post.file.isEmpty() ? QString()
                                        : QFileInfo(post.file).fileName();
    if (label.isEmpty() && !path.isEmpty())
        label = QFileInfo(path).fileName();
    if (label.isEmpty())
        label = post.postUrl.isEmpty() ? tr("post #%1").arg(post.id)
                                       : post.postUrl;
    HydrusTagsDialog dlg(hydrusConfigFromSettings(m_settings), label, path, md5,
                         this);
    dlg.exec();
}

void MainWindow::applyStrayTagEdits(const QVector<StrayTagEdit> &edits) {
    if (edits.isEmpty())
        return;
    auto norm = [](const QString &s) {
        QStringList kept;
        for (const QString &t : s.simplified().toLower().split(
                 QChar::Space, Qt::SkipEmptyParts)) {
            if (!kept.contains(t))
                kept.append(t);
        }
        return kept;
    };
    QVector<QPair<QString, QJsonObject>> written; // absFile -> new sidecar
    int skippedArchive = 0;
    int skippedWrite = 0;
    for (const StrayTagEdit &e : edits) {
        if (e.absFile.isEmpty())
            continue;
        const QFileInfo fi(e.absFile);
        const QString sidecar = fi.absolutePath() + u'/' +
                                fi.completeBaseName() +
                                QStringLiteral(".json");
        if (refuseArchiveWrite(sidecar, effectiveDownloadDir(m_settings))) {
            ++skippedArchive;
            continue;
        }
        QJsonObject obj;
        {
            QFile f(sidecar);
            if (f.open(QIODevice::ReadOnly)) {
                QJsonParseError err;
                const QJsonDocument doc =
                    QJsonDocument::fromJson(f.readAll(), &err);
                if (err.error == QJsonParseError::NoError && doc.isObject())
                    obj = doc.object();
            }
        }
        const QStringList artist = norm(e.artist);
        const QStringList character = norm(e.character);
        const QStringList copyright = norm(e.copyright);
        const QStringList general = norm(e.general);
        const QStringList meta = norm(e.meta);
        QStringList unionTags;
        for (const QStringList &bucket :
             {artist, character, copyright, general, meta})
            for (const QString &t : bucket)
                if (!unionTags.contains(t))
                    unionTags.append(t);
        // Write both the flat union and the buckets sidecar keys (order:
        // artist/character/copyright/general/meta, parseStray bucketed
        // path). Stale buckets are replaced, never merged.
        for (const char *k : {"tags_artist", "tags_character", "tags_copyright",
                              "tags_general", "tags_meta", "stray_tags"})
            obj.remove(QString::fromLatin1(k));
        obj[QStringLiteral("tags")] = unionTags.join(u' ');
        if (!artist.isEmpty())
            obj[QStringLiteral("tags_artist")] = artist.join(u' ');
        if (!character.isEmpty())
            obj[QStringLiteral("tags_character")] = character.join(u' ');
        if (!copyright.isEmpty())
            obj[QStringLiteral("tags_copyright")] = copyright.join(u' ');
        if (!general.isEmpty())
            obj[QStringLiteral("tags_general")] = general.join(u' ');
        if (!meta.isEmpty())
            obj[QStringLiteral("tags_meta")] = meta.join(u' ');
        obj[QStringLiteral("stray")] = true;
        {
            QFile f(sidecar);
            if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                ++skippedWrite;
                continue;
            }
            f.write(QJsonDocument(obj).toJson(QJsonDocument::Indented));
        }
        // Touch the media so rescans reparse the sidecar (same
        // scan-cache lifecycle as the single editor).
        if (!refuseArchiveWrite(e.absFile, effectiveDownloadDir(m_settings))) {
            QFile touch(e.absFile);
            if (touch.open(QIODevice::ReadWrite))
                touch.setFileTime(QDateTime::currentDateTime(),
                                  QFileDevice::FileModificationTime);
        }
        written.append(qMakePair(e.absFile, obj));
    }
    if (written.isEmpty()) {
        statusBar()->showMessage(
            skippedArchive ? tr("Library archives are read-only")
                           : tr("Cannot write sidecars"),
            6000);
        return;
    }
    applyStrayTagUpdates(written);
    QString msg = tr("Saved tags for %1 stray(s)").arg(written.size());
    if (skippedArchive || skippedWrite)
        msg += tr(" (skipped %1)").arg(skippedArchive + skippedWrite);
    statusBar()->showMessage(msg, 4000);
}

void MainWindow::applyStrayTagUpdates(
    const QVector<QPair<QString, QJsonObject>> &written) {
    if (written.isEmpty())
        return;
    auto canon = [](const QString &path) {
        QString c = QFileInfo(path).canonicalFilePath();
        return c.isEmpty() ? QFileInfo(path).absoluteFilePath() : c;
    };
    QHash<QString, Post> updates; // canonical absFile -> parsed tag fields
    for (const auto &[abs, obj] : written) {
        Post parsed;
        if (MetaParser::parse(obj, parsed))
            updates.insert(canon(abs), parsed);
    }
    if (updates.isEmpty())
        return;
    // Only tag/rating presence travels: identity (site/id/post URL, local
    // path) always stays with the row being patched.
    auto mergeTags = [&updates, &canon](Post &p) {
        if (p.isRemote || p.absFile.isEmpty())
            return false;
        const auto it = updates.constFind(canon(p.absFile));
        if (it == updates.constEnd())
            return false;
        const Post &u = it.value();
        p.tags = u.tags;
        p.tagsArtist = u.tagsArtist;
        p.tagsCharacter = u.tagsCharacter;
        p.tagsCopyright = u.tagsCopyright;
        p.tagsGeneral = u.tagsGeneral;
        p.tagsMeta = u.tagsMeta;
        p.customTags = u.customTags;
        p.untyped = u.untyped;
        p.rating = u.rating;
        p.ratingInferred = u.ratingInferred;
        p.hasMeta = true;
        return true;
    };
    // Live index (local tabs re-query it) and every tab's stored rows
    // (favourites + downloads merges keep their own copies).
    QVector<Post> posts = m_index.posts();
    for (Post &p : posts)
        mergeTags(p);
    m_index.setPosts(posts);
    for (BrowserTab *t : m_tabs) {
        for (Post &p : t->localPosts)
            mergeTags(p);
        for (Post &p : t->browseMerged)
            mergeTags(p);
    }
    for (Post &p : m_current)
        mergeTags(p);
    // Favourites rows linked to the edited files: persist the flat union
    // too, otherwise search (which reads Post::tags) stays blind.
    if (m_favDb.isOpen()) {
        QHash<QString, const Post *> byPath;
        for (auto it = updates.constBegin(); it != updates.constEnd(); ++it)
            byPath.insert(it.key(), &it.value());
        const auto paths = m_favDb.localPaths();
        QHash<QPair<QString, qint64>, FavDb::TagSplit> splits;
        for (auto it = paths.constBegin(); it != paths.constEnd(); ++it) {
            const auto hit = byPath.constFind(canon(it.value()));
            if (hit == byPath.constEnd())
                continue;
            const Post &u = *hit.value();
            FavDb::TagSplit s;
            s.tags = u.tags;
            s.artist = u.tagsArtist;
            s.character = u.tagsCharacter;
            s.copyright = u.tagsCopyright;
            s.general = u.tagsGeneral;
            s.meta = u.tagsMeta;
            s.untyped = u.untyped;
            splits.insert(it.key(), s);
        }
        if (!splits.isEmpty())
            m_favDb.setTagSplits(splits);
    }
    // Current tab: favourites/local filter from their row lists, a
    // downloads merge goes through the browse filter.
    if (BrowserTab *t = currentTab()) {
        if (t->isFavourites || !t->isRemote) {
            if (!t->isFavourites)
                t->localPosts = m_index.posts();
            rebuildCombos();
            applyFilter();
        } else {
            m_browseMerged = t->browseMerged;
            applyBrowseFilter();
        }
    }
    // Viewer: keep the frame, update the metadata panel + tags.
    if (m_pages && m_pages->currentIndex() == 1) {
        const QPixmap keep = m_viewer->currentPixmap();
        m_viewer->setPosts(m_current);
        if (!keep.isNull())
            m_viewer->showThumbPreview(keep);
        int row = -1;
        for (int i = 0; i < m_current.size(); ++i) {
            if (!m_current.at(i).isRemote &&
                updates.contains(canon(m_current.at(i).absFile))) {
                row = i;
                break;
            }
        }
        if (row >= 0)
            m_viewer->showRow(row);
        m_viewer->refreshCurrentPost();
    }
    maybeClassify();
}

