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

void MainWindow::relinkFavourites() {
    if (!m_favDb.isOpen())
        return;
    const QVector<FavDb::Unlinked> missing = m_favDb.unlinked();
    if (missing.isEmpty())
        return;
    // Search roots: current download folder plus legacy defaults,
    // where files downloaded by older versions live (pre-rename
    // Pictures/booru-explorer included).
    QStringList roots;
    roots << effectiveDownloadDir(m_settings);
    const QString pics = QStandardPaths::writableLocation(
        QStandardPaths::PicturesLocation);
    for (const QString &seg :
         {QStringLiteral("/box"), QStringLiteral("/booru-explorer")}) {
        const QString legacy = pics + seg;
        if (QDir(legacy).exists() && !roots.contains(legacy))
            roots << legacy;
    }
    // One listing for the whole tree (not one per missing row): fresh
    // imports can leave hundreds unlinked, and entryList per row stalls
    // tab switches. Folder-agnostic on purpose: download folders moved
    // from <Label> to <host> over versions (and label renames orphan
    // old folders), so match the _<md5> suffix in ANY site folder.
    // Same bytes = same md5, so cross-folder hits are still correct.
    QHash<QString, QString> byStem; // stem -> abs path (first wins)
    for (const QString &root : roots) {
        const QDir rootDir(root);
        if (!rootDir.exists())
            continue;
        for (const QString &siteDir :
             rootDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            const QDir dir(rootDir.absoluteFilePath(siteDir));
            for (const QFileInfo &fi : dir.entryInfoList(QDir::Files)) {
                const QString stem = fi.completeBaseName();
                // Any prefixed stem qualifies (<host>_<md5>, legacy
                // <Label>_<md5> and the underscore-leading _Stray_<md5>
                // alike).
                if (stem.contains(u'_') && !byStem.contains(stem))
                    byStem.insert(stem, fi.absoluteFilePath());
            }
        }
    }
    QHash<QPair<QString, qint64>, QString> found;
    for (const FavDb::Unlinked &u : missing) {
        const QString md5 = LosnImport::md5FromFileUrl(u.fileUrl);
        if (md5.isEmpty())
            continue;
        // <tag>_<md5>.<any ext>, in any folder
        for (auto s = byStem.begin(); s != byStem.end(); ++s) {
            if (s.key().endsWith(u'_' + md5)) {
                found.insert({u.site, u.id}, s.value());
                break;
            }
        }
    }
    const int n = m_favDb.setLocalPaths(found);
    // Regeneration: favourited strays whose file is gone but whose source
    // URL survives re-download into place (kept favourites only). Only
    // rows that previously had a file (recorded local_path now missing):
    // never-downloaded rows are not the app's to fetch unasked. Once
    // per session per row; failures simply retry next launch. Never
    // preempts an active download batch.
    QVector<Post> regen;
    if (m_downloads && !m_downloads->isActive()) {
        const QHash<QPair<QString, qint64>, QString> recorded =
            m_favDb.localPaths();
        for (const FavDb::Unlinked &u : missing) {
            if (u.site != Stray::dirName() || u.fileUrl.isEmpty() ||
                !RemotePosts::isHttpUrl(u.fileUrl))
                continue;
            if (found.contains({u.site, u.id}))
                continue;
            if (recorded.value(QPair<QString, qint64>(u.site, u.id)).isEmpty())
                continue; // never had a file: nothing was deleted
            const QString key =
                u.site + u'#' + QString::number(u.id) + u'|' + u.fileUrl;
            if (m_regenTried.contains(key))
                continue;
            m_regenTried.insert(key);
            Post p;
            p.isRemote = true;
            p.siteName = u.site;
            p.id = u.id;
            p.fileUrl = u.fileUrl;
            p.ext = u.ext;
            QJsonObject raw;
            raw[QStringLiteral("stray")] = true;
            raw[QStringLiteral("tags")] = u.tags;
            if (!u.ext.isEmpty())
                raw[QStringLiteral("file_ext")] = u.ext;
            p.rawJson = raw;
            regen.append(p);
        }
    }
    if (!regen.isEmpty()) {
        downloadPosts(regen);
        statusBar()->showMessage(
            tr("Re-downloading %1 missing stray file(s)…").arg(regen.size()),
            8000);
    } else if (n > 0) {
        statusBar()->showMessage(
            tr("Linked %1 favourites to files on disk.").arg(n), 6000);
    }
}

// Manual re-pull (File menu): favourited files deleted outside the app
// (moves already healed by relinkFavourites above). Only rows that
// previously had a file (recorded local_path now missing) qualify:
// never-downloaded favourites are not the app's to fetch unasked. Rows
// with no download source stay hidden in the grid (never marked
// downloaded) until the file returns; the prompt says so when any exist.
void MainWindow::redownloadMissingFiles() {
    if (!m_favDb.isOpen())
        return;
    const QHash<QPair<QString, qint64>, QString> paths = m_favDb.localPaths();
    // Entry behind a file URL: exact host first, then media CDNs living
    // next door (subdomain of an entry host). The entry picks the folder,
    // referer, cookies and proxy, so the re-pull lands where the first
    // download did (stamping siteKey is what makes that happen).
    auto keyForFileHost = [](const QString &fileHost) -> QString {
        if (fileHost.isEmpty())
            return {};
        const QMap<QString, QString> hosts = RemotePosts::hostKeys();
        if (hosts.contains(fileHost))
            return hosts.value(fileHost);
        for (auto it = hosts.begin(); it != hosts.end(); ++it) {
            if (fileHost.endsWith(u'.' + it.key()))
                return it.value();
        }
        return {};
    };
    // Re-pull the stored DB rows themselves (not bare reconstructions),
    // so names, tags and the sidecar come back complete.
    QVector<Post> redl;
    int unregen = 0;
    for (Post p : m_favDb.list()) {
        if (p.siteName == Stray::dirName())
            continue; // stray regen path owns these (retries next launch)
        const QString recorded = paths.value(favIdentity(p));
        if (recorded.isEmpty() || QFileInfo::exists(recorded))
            continue; // never downloaded, or still on disk
        if (p.fileUrl.isEmpty() || !RemotePosts::isHttpUrl(p.fileUrl)) {
            unregen++;
            continue;
        }
        const QString key =
            keyForFileHost(QUrl(p.fileUrl).host().toLower());
        const BooruSite *entry =
            key.isEmpty() ? nullptr : RemoteSource::siteFor(key);
        if (!entry || entry->baseUrl.isEmpty()) {
            unregen++; // URL but no configured booru to pull it through
            continue;
        }
        p.isRemote = true;
        p.siteKey = key;
        redl.append(p);
    }
    if (redl.isEmpty()) {
        statusBar()->showMessage(tr("No missing downloaded files"), 6000);
        return;
    }
    QString q = tr("%1 favourited file(s) are missing from disk. "
                   "Redownload them?")
                    .arg(redl.size());
    if (unregen > 0)
        q += tr("\n\n%1 other(s) have no download source and stay hidden.")
                 .arg(unregen);
    if (QMessageBox::question(this, tr("Missing downloads"), q,
                              QMessageBox::Yes | QMessageBox::No,
                              QMessageBox::Yes) != QMessageBox::Yes)
        return;
    downloadPosts(redl);
    statusBar()->showMessage(
        tr("Re-downloading %1 missing file(s)…").arg(redl.size()), 8000);
}

