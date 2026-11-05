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

void MainWindow::applyBrowseFilter() {
    BrowserTab *t = currentTab();
    QVector<Post> posts = m_browseMerged;
    const QSet<QString> banned(m_settings.blacklist.begin(),
                               m_settings.blacklist.end());
    if (!banned.isEmpty()) {
        // Safety net (server-side -exclusion is in the query already).
        posts.erase(std::remove_if(posts.begin(), posts.end(),
                                   [&](const Post &p) { return blacklisted(p, banned); }),
                    posts.end());
    }
    if (m_settings.hideAi) {
        // Safety net for merged pages (server-side -ai_* is in the query).
        posts.erase(std::remove_if(posts.begin(), posts.end(),
                                   [](const Post &p) { return isAiPost(p); }),
                    posts.end());
    }
    if (t && t->rating != QStringLiteral("all")) {
        posts.erase(std::remove_if(posts.begin(), posts.end(),
                                   [&](const Post &p) { return p.rating != t->rating; }),
                    posts.end());
    }
    // Media-kind net (filetype: tags + bare media tags, which ride the
    // same ext meaning): engines without verified support get the tokens
    // stripped server-side, so filter client-side here. Danbooru-engine
    // rows were filtered natively server-side (including ugoira zips,
    // which no ext set covers), so the net leaves them alone.
    if (t) {
        QList<QSet<QString>> ftInc, ftExc;
        const QStringList ftToks =
            t->activeTags + t->searchQuery.split(QChar::Space, Qt::SkipEmptyParts);
        for (const QString &tok : ftToks) {
            const bool neg = tok.startsWith(u'-') && tok.size() > 1;
            const QString base = neg ? tok.mid(1) : tok;
            if (isFiletypeToken(base)) {
                (neg ? ftExc : ftInc).append(filetypeExtSet(tok));
                continue;
            }
            const QString low = base.toLower();
            if (low == QStringLiteral("animated"))
                (neg ? ftExc : ftInc).append(filetypeExtSet(mediaAnimatedToken()));
            else if (low == QStringLiteral("animated_gif"))
                (neg ? ftExc : ftInc).append(filetypeExtSet(mediaGifToken()));
            else if (low == QStringLiteral("video"))
                (neg ? ftExc : ftInc).append(filetypeExtSet(mediaVideoToken()));
        }
        if (!ftInc.isEmpty() || !ftExc.isEmpty()) {
            posts.erase(std::remove_if(posts.begin(), posts.end(),
                                       [&](const Post &p) {
                                           const QString key =
                                               !p.siteKey.isEmpty()
                                                   ? p.siteKey
                                                   : RemotePosts::keyForLabel(
                                                         p.siteName);
                                           const BooruSite *site =
                                               key.isEmpty() ? nullptr
                                               : RemoteSource::siteFor(key);
                                           if (site &&
                                               site->engine ==
                                                   BooruEngine::Danbooru)
                                               return false;
                                           return !filetypeExtOk(p.ext, ftInc,
                                                                 ftExc);
                                       }),
                        posts.end());
        }
    }
    sortPosts(posts, t ? t->sort : m_sort);
    m_current = posts;
    fillModel();
    rebuildBrowseSidebar();
    updateResultLabel();
    updateTabTitle(m_curTab);
    scheduleHybooruHydration();
}

