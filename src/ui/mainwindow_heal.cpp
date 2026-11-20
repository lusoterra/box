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
#include <QDirIterator>
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
#include <QRegularExpression>
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

const MainWindow::DownloadedIndex &MainWindow::downloadDiskIndex() {
    const QString root =
        QDir(effectiveDownloadDir(m_settings)).absolutePath();
    const qint64 stamp = QFileInfo(root).lastModified().toMSecsSinceEpoch();
    if (m_dlIndex.stamp == stamp && m_dlIndex.root == root)
        return m_dlIndex;
    m_dlIndex = DownloadedIndex();
    m_dlIndex.root = root;
    m_dlIndex.stamp = stamp;
    static const QRegularExpression md5Stem(
        QStringLiteral("_(?P<md5>[0-9a-f]{32})$"),
        QRegularExpression::CaseInsensitiveOption);
    QDirIterator it(root, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QFileInfo fi(it.next());
        const QString suffix = fi.suffix().toLower();
        if (suffix == QStringLiteral("json") ||
            suffix == QStringLiteral("part") ||
            suffix == QStringLiteral("tmp"))
            continue;
        const QString stem = fi.completeBaseName();
        m_dlIndex.stems.insert(stem);
        const auto m = md5Stem.match(stem);
        if (m.hasMatch())
            m_dlIndex.md5s.insert(
                m.captured(QStringLiteral("md5")).toLower());
    }
    return m_dlIndex;
}

bool MainWindow::isPostDownloaded(const Post &post) {
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
    // Remote posts: the file itself decides. Prefer the content hash
    // (folder-agnostic: host folders, legacy label folders, moved files),
    // then the exact planned filename for hash-less posts.
    const DownloadedIndex &idx = downloadDiskIndex();
    QString md5 = post.md5.trimmed().toLower();
    static const QRegularExpression hex32(
        QStringLiteral("^[0-9a-f]{32}$"));
    if (!hex32.match(md5).hasMatch())
        md5 = LosnImport::md5FromFileUrl(post.fileUrl);
    if (md5.isEmpty()) {
        md5 = post.rawJson.value(QStringLiteral("md5")).toString().toLower();
        if (md5.isEmpty())
            md5 = post.rawJson.value(QStringLiteral("checksumMD5"))
                      .toString()
                      .toLower();
    }
    if (!md5.isEmpty() && idx.md5s.contains(md5))
        return true;
    Post probe = post;
    if (!md5.isEmpty() &&
        probe.rawJson.value(QStringLiteral("md5")).toString().isEmpty())
        probe.rawJson[QStringLiteral("md5")] = md5;
    const QList<DownloadJob> jobs =
        DownloadManager::plan({probe}, idx.root);
    if (!jobs.isEmpty()) {
        const QString name = jobs.first().fileName;
        const qsizetype dot = name.lastIndexOf(u'.');
        if (idx.stems.contains(dot > 0 ? name.left(dot) : name))
            return true;
    }
    return false;
}

void MainWindow::updateViewerBadges() {
    if (!m_viewer || !m_viewer->hasCurrent())
        return;
    const Post p = m_viewer->currentPost();
    const auto [site, pid] = favIdentity(p);
    const bool faved =
        m_favDb.isOpen() && m_favDb.isFavourite(site, pid);
    const bool downloaded = isPostDownloaded(p);
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
    const int row =
        found >= 0 ? found
                   : qMin(m_viewer->currentRow(),
                          static_cast<int>(m_current.size()) - 1);
    m_viewer->setPosts(m_current);
    m_viewer->showRow(row);
    const Post &p = m_current.at(row);
    const auto [site, pid] = favIdentity(p);
    m_viewer->setFaved(m_favDb.isOpen() && m_favDb.isFavourite(site, pid));
    const QString reason = refreshReason(p);
    m_viewer->setRefreshable(reason.isEmpty(), reason);
    updateViewerBadges();
}

