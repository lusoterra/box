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

void MainWindow::refreshPost(const Post &post) {
    RefreshTarget target = resolveRefreshTarget(post);
    if (!target.valid()) {
        statusBar()->showMessage(refreshReason(post), 6000);
        return;
    }
    // Note: target.creds already comes from the configured booru entry.
    m_refreshPost = post;
    m_refreshTarget = target;
    statusBar()->showMessage(tr("Refreshing tags…"));
    m_refresher->refresh(target, ++m_refreshReq);
}

void MainWindow::onRefreshFailed(qint64 reqId, const QString &error) {
    if (reqId != m_refreshReq)
        return; // superseded: a newer refresh owns the UI now
    if (m_viewer)
        m_viewer->detailFetchFailed(m_refreshPost);
    statusBar()->showMessage(tr("Refresh failed: %1").arg(error), 8000);
}

Post remapFresh(const QJsonObject &raw, const RefreshTarget &t) {
    // Engine-based so custom instances map like their family presets.
    using enum BooruEngine;
    switch (RemotePosts::engineForSiteKey(t.siteKey)) {
    case Danbooru:
        return RemotePosts::fromDanbooru(raw, t.baseUrl);
    case Gelbooru:
    case Safebooru:
        return RemotePosts::fromGelbooru(raw, t.baseUrl);
    case Szurubooru:
        return RemotePosts::fromSzuru(raw, t.baseUrl);
    case Hybooru:
        return RemotePosts::fromHybooru(raw, t.baseUrl);
    case Nuuru:
        return RemotePosts::fromNuuru(raw, t.baseUrl);
    case Derpibooru:
        return RemotePosts::fromDerpibooru(raw, t.baseUrl);
    case Twibooru:
        return RemotePosts::fromTwibooru(raw, t.baseUrl);
    case Shimmie: {
        QMap<QString, QString> attrs;
        for (auto it = raw.begin(); it != raw.end(); ++it)
            attrs.insert(it.key(), it.value().toVariant().toString());
        return RemotePosts::fromShimmie(attrs, t.baseUrl);
    }
    case Moebooru:
        return RemotePosts::fromMoebooru(raw, t.baseUrl);
    }
    return RemotePosts::fromMoebooru(raw, t.baseUrl);
}

// Sidecar the scanner would have used for this local file.
QString localSidecarFor(const Post &p) {
    const QFileInfo fi(p.absFile);
    const QString flat = p.absFile + QStringLiteral(".json");
    if (QFile::exists(flat))
        return flat;
    const QString stem =
        fi.absolutePath() + u'/' + fi.completeBaseName() + QStringLiteral(".json");
    if (QFile::exists(stem))
        return stem;
    return {};
}

void MainWindow::onRefreshDone(qint64 reqId,
                                      const QJsonObject &raw) {
    if (reqId != m_refreshReq)
        return; // superseded: an older flight landing late must not
                // remap (wrong-site data used to replace the grid row).
    const Post fresh = remapFresh(raw, m_refreshTarget);
    const Post orig = m_refreshPost;
    const QString siteKey = m_refreshTarget.siteKey;
    int tagCount = 0;

    if (orig.isRemote) {
        // In-memory update (+ downloaded copy's sidecar, if present).
        Post merged = fresh;
        merged.category = orig.category;
        merged.siteKey = siteKey; // refresh targets resolve exactly, twice
        // Repaint the open viewer even if the page-index timing made the
        // block below skip it (the first-open HyBooru tags bug).
        if (m_viewer)
            m_viewer->applyRefreshedPost(merged);
        auto replaceIn = [&](QVector<Post> &vec) {
            for (Post &p : vec) {
                if (p.isRemote && p.siteName == orig.siteName && p.id == orig.id)
                    p = merged;
            }
        };
        replaceIn(m_current);
        replaceIn(m_browseMerged);
        if (BrowserTab *t = currentTab())
            replaceIn(t->browseMerged);
        tagCount = merged.tags.split(QChar::Space, Qt::SkipEmptyParts).size();
        fillModel();
        rebuildBrowseSidebar();
        updateResultLabel();
        if (m_pages->currentIndex() == 1) {
            // Refresh must not blank the viewer: keep the displayed
            // frame as the pending thumb until the full file reloads.
            const QPixmap keep = m_viewer->currentPixmap();
            m_viewer->setPosts(m_current);
            if (!keep.isNull())
                m_viewer->showThumbPreview(keep);
            // Re-show the same post if still present.
            for (int i = 0; i < m_current.size(); ++i) {
                const Post &p = m_current[i];
                if (p.isRemote && p.siteName == orig.siteName && p.id == orig.id) {
                    m_viewer->showRow(i);
                    break;
                }
            }
        }
        // Downloaded copy's sidecar shares the filename convention.
        // Folders moved from <Label> to <host> over versions: check the
        // host path first, then the legacy label path.
        const QString tag = RemotePosts::fsTagForPost(orig);
        const QString stem =
            RemotePosts::downloadFileName(tag, raw).split(u'.').first();
        QString sidecar = effectiveDownloadDir(m_settings) + u'/' + tag +
                          u'/' + stem + QStringLiteral(".json");
        if (!QFile::exists(sidecar) && tag != orig.siteName) {
            const QString legacyStem =
                RemotePosts::downloadFileName(orig.siteName, raw)
                    .split(u'.')
                    .first();
            sidecar = effectiveDownloadDir(m_settings) + u'/' + orig.siteName +
                      u'/' + legacyStem + QStringLiteral(".json");
        }
        if (QFile::exists(sidecar)) {
            QFile f(sidecar);
            if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
                f.write(QJsonDocument(raw).toJson(QJsonDocument::Indented)); // jq-readable
        }
    } else {
        // Local file: rewrite its sidecar, then re-parse through the normal path.
        // Archive roots (outside the download dir) are read-only like
        // everywhere else (no Delete there either): merge via a temp copy
        // so Syncthing-style mirrors never see local additions.
        const QString sidecar = localSidecarFor(orig);
        if (sidecar.isEmpty()) {
            statusBar()->showMessage(tr("Refresh: sidecar not writable"), 6000);
            return;
        }
        const QString dlRoot =
            QDir(effectiveDownloadDir(m_settings)).canonicalPath();
        const QString absDir =
            QDir(QFileInfo(orig.absFile).absolutePath()).canonicalPath();
        const bool writable =
            !dlRoot.isEmpty() &&
            (absDir == dlRoot || absDir.startsWith(dlRoot + u'/'));
        QString parsePath = sidecar;
        QTemporaryFile tmp;
        if (!writable) {
            if (!tmp.open())
                return;
            QFile src(sidecar);
            if (!src.open(QIODevice::ReadOnly))
                return;
            tmp.write(src.readAll());
            tmp.flush();
            parsePath = tmp.fileName();
        }
        // Hard safeguard: never write a sidecar into a library archive.
        // parsePath is the temp copy for archives (passes), or the real
        // sidecar in the download dir (passes). Catches any miscalc of
        // the `writable` flag above.
        if (refuseArchiveWrite(parsePath, effectiveDownloadDir(m_settings))) {
            statusBar()->showMessage(
                tr("Archive is read-only: tags refreshed for this session "
                   "only"),
                6000);
            return;
        }
        if (!writeRefreshedSidecar(parsePath, raw, siteKey)) {
            statusBar()->showMessage(tr("Refresh: sidecar not writable"), 6000);
            return;
        }
        if (!writable)
            statusBar()->showMessage(
                tr("Archive is read-only: tags refreshed for this session "
                   "only"),
                6000);
        QFile f(parsePath);
        Post updated;
        if (!f.open(QIODevice::ReadOnly) ||
            !MetaParser::parse(
                QJsonDocument::fromJson(f.readAll()).object(), updated)) {
            statusBar()->showMessage(tr("Refresh: re-parse failed"), 6000);
            return;
        }
        updated.file = orig.file;
        updated.absFile = orig.absFile;
        updated.ext = orig.ext;
        updated.category = orig.category;
        tagCount = updated.tags.split(QChar::Space, Qt::SkipEmptyParts).size();
        QVector<Post> posts = m_index.posts();
        for (Post &p : posts) {
            if (!p.isRemote && p.absFile == orig.absFile)
                p = updated;
        }
        m_index.setPosts(posts);
        if (BrowserTab *t = currentTab(); t && !t->isRemote && !t->isFavourites)
            t->localPosts = posts;
        rebuildCombos();
        rebuildSidebar();
        applyFilter();
        if (m_pages->currentIndex() == 1) {
            m_viewer->setPosts(m_current);
            for (int i = 0; i < m_current.size(); ++i) {
                if (!m_current[i].isRemote && m_current[i].absFile == orig.absFile) {
                    m_viewer->showRow(i);
                    break;
                }
            }
        }
    }
    statusBar()->showMessage(tr("Tags refreshed (%1 tags)").arg(tagCount),
                             6000);
    updateTabTitle(m_curTab);
}

