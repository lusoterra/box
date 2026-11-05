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

void MainWindow::fillModel() {
    // Twins (remote/local copies of one post) card twice and make
    // viewer stepping stutter on "the same post", so collapse them
    // here at the single funnel every grid rebuild passes through.
    // Local rows whose file is gone can render nothing (the thumb
    // loader skips missing files below): drop them instead of leaving
    // blank cards. Remote rows always stay: they fall back to their
    // URLs (or a viewer refresh when only the post link is left).
    {
        QSet<QString> seen;
        QVector<Post> unique;
        unique.reserve(m_current.size());
        for (const Post &post : m_current) {
            if (!post.isRemote &&
                (post.absFile.isEmpty() || !QFileInfo::exists(post.absFile)))
                continue;
            const QString k = dedupeKeyFor(post);
            if (k.isEmpty() || !seen.contains(k)) {
                if (!k.isEmpty())
                    seen.insert(k);
                unique.append(post);
            }
        }
        m_current.swap(unique);
    }
    m_model->clear();
    // One query per refill (not per row): favourite links back the
    // downloaded/favourited marks below.
    const QHash<QPair<QString, qint64>, QString> favLocal =
        m_favDb.isOpen() ? m_favDb.localPaths()
                         : QHash<QPair<QString, qint64>, QString>();
    const auto favByPath = favKeysByPath(favLocal);
    // Stamp the download state once per refill: the cache policy lets
    // explicitly downloaded NSFW media persist under "SFW only" (a bare
    // favourite does not), and the thumbnail queue + viewer copies below
    // must carry the same flag.
    for (Post &p : m_current)
        p.downloaded = isPostDownloaded(p, &favLocal);
    for (const Post &p : m_current) {
        auto *item = new QStandardItem();
        QString key;
        if (p.isRemote) {
            // The info card already shows #id: display the file name here
            // so the two lines never duplicate each other. Extensionless
            // endpoints (Nuuru /file) get a synthesized readable name;
            // downloads are unaffected (they keep site_md5 naming).
            QString name = RemotePosts::displayFileName(p);
            if (name.isEmpty() && p.id)
                name = QStringLiteral("#%1").arg(p.id);
            item->setData(name, Qt::DisplayRole);
            key = QStringLiteral("url|") + RemotePosts::thumbUrlFor(p);
        } else {
            item->setData(QFileInfo(p.file).fileName(), Qt::DisplayRole);
            const QFileInfo fi(p.absFile);
            key = p.absFile + u'|' + QString::number(fi.lastModified().toMSecsSinceEpoch());
        }
        // Pixmaps window with the viewport (see requestVisibleThumbs):
        // every row starts on the shared placeholder; the ensure pass
        // below paints cached thumbs onto near-visible rows only.
        item->setData(m_placeholder, Qt::DecorationRole);
        item->setData(p.rating, Qt::UserRole + 1);
        QString kind;
        if (isVideoExt(p.ext))
            kind = QStringLiteral("▶");
        else if (p.ext == QStringLiteral("gif"))
            kind = QStringLiteral("GIF");
        item->setData(kind, Qt::UserRole + 2);
        item->setData(QStringLiteral("%1%2")
                          .arg(p.score > 0 ? QStringLiteral("+") : QStringLiteral(""))
                          .arg(p.score),
                      Qt::UserRole + 3);
        item->setData(key, Qt::UserRole + 4);
        item->setData(p.isRemote ? QString() : p.absFile, Qt::UserRole + 5);
        // Drag payload: remote stills/GIFs carry the configured
        // direct/post URL (per-booru override included); local stills/GIFs
        // drag their own file. Videos are excluded on purpose.
        QString dragUrl;
        if (!isVideoExt(p.ext)) {
            dragUrl = p.isRemote
                          ? RemotePosts::dragUrlFor(p, m_settings.dragLinkMode)
                          : (p.absFile.isEmpty()
                                 ? QString()
                                 : QUrl::fromLocalFile(p.absFile).toString());
        }
        item->setData(dragUrl, Qt::UserRole + 20);
        // Local posts carry a hash id, not a booru number: show a dash.
        item->setData(p.isRemote && p.id ? QStringLiteral("#%1").arg(p.id)
                                         : QStringLiteral("-"),
                      Qt::UserRole + 6);
        // Origin badge: the configured entry's text + color (never the
        // engine-family code) plus its favicon. Browsed posts carry
        // their entry key; favourites and older rows resolve by label
        // instead.
        QString badge, badgeColor, badgeKey;
        QPixmap badgeFav;
        if (const BooruSite *entry = RemotePosts::entryForPost(p)) {
                badge = entry->shortName.isEmpty() ? p.siteShort
                                                   : entry->shortName;
                badgeColor = entry->badgeColor;
                badgeKey = entry->key;
                // Sync hit from memory/disk; a miss queues a fetch and
                // lands via onFaviconReady (rows update in place).
                badgeFav = m_favicons->iconFor(entry->key, entry->baseUrl)
                               .pixmap(16, 16);
            } else if (p.isRemote) {
                badge = p.siteShort;
            }
        item->setData(badge, Qt::UserRole + 7);
        item->setData(badgeColor, Qt::UserRole + 8);
        if (!badgeFav.isNull())
            item->setData(badgeFav, Qt::UserRole + 10);
        item->setData(badgeKey, Qt::UserRole + 11);
        item->setData(isAiPost(p), Qt::UserRole + 9); // grid "AI" chip
        // Comments/annotations pills (parsed where the engine exposes
        // counts/stamps/flags; viewer meta ORs in on arrival).
        item->setData(p.hasComments || p.commentCount > 0, Qt::UserRole + 14);
        item->setData(p.hasNotes, Qt::UserRole + 15);
        // Downloaded (under the download folder / linked favourite
        // file) and favourited marks, next to the AI chip.
        item->setData(isFavMarked(p, favLocal, favByPath),
                      Qt::UserRole + 12);
        item->setData(p.downloaded, Qt::UserRole + 13);
        item->setEditable(false);
        // The tooltip stays on the post page even after a download
        // links the row to disk; strays (no post URL) keep the file.
        item->setToolTip(!p.postUrl.isEmpty() ? p.postUrl : p.file);
        m_model->appendRow(item);
    }
    syncViewerToGrid();
    requestVisibleThumbs();
}

// The grid behind an open viewer changed (unfavourite, delete,
// filter): point the viewer at the same post in the new list so
// deleted rows stop showing as blank ghosts and twins stop making
// stepping stutter. Untouched when the current post sits at the
// same index, so playback never restarts for unrelated refills.
