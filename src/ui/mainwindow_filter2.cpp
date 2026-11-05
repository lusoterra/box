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

void MainWindow::rebuildTagTab(QListWidget *list,
                               const QVector<QPair<QString, int>> &entries,
                               const QString &bucket) {
    list->clear();
    const QColor color = UiIcons::tagBucketColor(bucket);
    for (const auto &[tag, n] : entries) {
        auto *it = new QListWidgetItem(QStringLiteral("%1  (%2)").arg(tag).arg(n));
        it->setData(Qt::UserRole, tag);
        it->setForeground(color);
        if (m_activeTags.contains(tag)) {
            QFont f = it->font();
            f.setBold(true);
            it->setFont(f);
        }
        list->addItem(it);
    }
}

void MainWindow::applyFilter() {
    if (isBrowse()) {
        applyBrowseFilter();
        return;
    }
    BrowserTab *t = currentTab();
    if (t && t->isFavourites) {
        // Client-side filter over the DB list (no index involved).
        QStringList inc, exc;
        splitIncExc(m_activeTags + m_searchQuery.split(QChar::Space, Qt::SkipEmptyParts),
                    inc, exc);
        const QSet<QString> banned(m_settings.blacklist.begin(),
                                   m_settings.blacklist.end());
        QVector<Post> out;
        for (const Post &p : t->localPosts) {
            if (m_rating != QStringLiteral("all") && p.rating != m_rating)
                continue;
            if (m_settings.hideAi && isAiPost(p))
                continue;
            if (blacklisted(p, banned))
                continue;
            if (matchesTokens(p, inc, exc))
                out.append(p);
        }
        sortPosts(out, m_sort);
        m_current = out;
        fillModel();
        rebuildBrowseSidebar();
        updateResultLabel();
        updateTabTitle(m_curTab);
        return;
    }
    QStringList tokens = m_activeTags + m_searchQuery.split(QChar::Space, Qt::SkipEmptyParts);
    for (const QString &b : m_settings.blacklist) {
        if (!b.isEmpty())
            tokens << QStringLiteral("-") + b;
    }
    if (m_settings.hideAi) {
        for (const QString &ai : aiContentTags())
            tokens << QStringLiteral("-") + ai;
    }
    const QueryResult r =
        m_index.query(tokens, m_rating, m_category, m_sort, 1, INT_MAX);
    m_current = r.posts;
    fillModel();
    updateResultLabel();
    updateTabTitle(m_curTab);
}

// Downloaded posts keep their booru: host/label folders resolve
// back to the entry behind the download (null = stray/library).
