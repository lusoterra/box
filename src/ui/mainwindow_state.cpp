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

void MainWindow::saveTabState() {
    BrowserTab *t = currentTab();
    if (!t)
        return;
    t->activeTags = m_activeTags;
    t->searchQuery = m_searchQuery;
    t->rating = m_rating;
    t->category = m_category;
    t->sort = m_sort;
    if (t->isFavourites) {
        // localPosts is the DB list, refreshed on activation; never overwrite.
    } else if (!t->isRemote) {
        t->localPosts = m_index.posts();
    } else {
        t->browseMerged = m_browseMerged;
    }
}

void MainWindow::onCompleterActivated(const QString &text) {
    QString cur = m_searchEdit->text();
    int pos = cur.lastIndexOf(u' ');
    QString prefix = (pos >= 0) ? cur.left(pos + 1) : QString();
    QString last = (pos >= 0) ? cur.mid(pos + 1) : cur;
    const QString dash = last.startsWith(u'-') ? QStringLiteral("-") : QString();
    m_searchEdit->setText(prefix + dash + text + u' ');
    // Accepting a suggestion (click or Enter - Enter never reaches
    // returnPressed while the popup is open) commits it like Enter:
    // pills, cleared omnibar, search. Multi-tag queries build across
    // repeated accepts, one search each.
    onSearchReturn();
}

void MainWindow::onRatingChanged() {
    m_rating = m_ratingBox->currentData().toString();
    // The box is a single-select shortcut for typing a rating: tag: a
    // fresh choice replaces any committed typed rating:/safety: tags
    // (they would AND with the box and empty everything).
    for (int i = m_activeTags.size() - 1; i >= 0; --i) {
        const QString t = m_activeTags.at(i);
        const QString low = t.toLower();
        if (isRatingToken(t) ||
            (low.startsWith(u'-') && isRatingToken(t.mid(1))))
            m_activeTags.removeAt(i);
    }
    if (isBrowse()) {
        commitSearch(); // refetch with the rating in the query + busy bar
        return;
    }
    commitFilter();
}

void MainWindow::normalizeTypedRating() {
    // A committed typed rating:/safety: tag clears the rating box to
    // "All": the tag carries the filter now, and the box must not
    // re-apply its own value on top of it.
    for (const QString &t : m_activeTags) {
        if (!isRatingToken(t))
            continue;
        if (m_rating != QStringLiteral("all")) {
            m_rating = QStringLiteral("all");
            m_ratingBox->blockSignals(true);
            m_ratingBox->setCurrentIndex(
                qMax(0, m_ratingBox->findData(m_rating)));
            m_ratingBox->blockSignals(false);
        }
        return;
    }
}

void MainWindow::onCategoryChanged() {
    m_category = m_categoryBox->currentData().toString();
    applyFilter();
}

void MainWindow::onMediaChanged() {
    // Downloads-only extension filter. Not a search token: the rows are
    // local files, so filtering is client-side and never refetches.
    if (!isDownloadsTab())
        return;
    BrowserTab *t = currentTab();
    if (!t)
        return;
    t->mediaFilter = m_mediaBox ? m_mediaBox->currentData().toString()
                                : QStringLiteral("all");
    if (isBrowse())
        applyBrowseFilter();
    else
        applyFilter();
    saveTabs();
}

void MainWindow::syncMediaBox() {
    // Follow the tab's own downloads media filter; every other tab resets
    // the hidden combo to All so a stale choice cannot leak across tabs.
    if (!m_mediaBox)
        return;
    const BrowserTab *t = currentTab();
    const QString want =
        (isDownloadsTab() && t) ? t->mediaFilter : QStringLiteral("all");
    const int idx = m_mediaBox->findData(want);
    m_mediaBox->blockSignals(true);
    m_mediaBox->setCurrentIndex(idx >= 0 ? idx : 0);
    m_mediaBox->blockSignals(false);
}

void MainWindow::normalizeTypedFiletype() {
    // A typed filetype: tag owns the filter (the box is only a shortcut).
    syncMediaBox();
}

void MainWindow::setBoxW(int w) {
    if (!m_delegate || !m_grid)
        return;
    m_requestedBoxW = qBound(96, w, 384);
    applyGridSnap();
}

