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

void MainWindow::visibleRowRange(int &lo, int &hi) const {
    lo = m_model ? m_model->rowCount() : 0;
    hi = -1;
    if (!m_grid || !m_model || m_model->rowCount() <= 0)
        return;
    const int vh = m_grid->viewport()->height();
    for (int r = 0; r < m_model->rowCount(); ++r) {
        const QRect ir = m_grid->visualRect(m_model->index(r, 0));
        if (!ir.isValid())
            continue;
        if (ir.bottom() >= 0 && ir.top() <= vh) {
            lo = qMin(lo, r);
            hi = qMax(hi, r);
        }
    }
    if (hi < 0)
        lo = 0;
}

void MainWindow::requestVisibleThumbs() {
    if (m_current.isEmpty() || m_pages->currentIndex() != 0)
        return;
    int lo = 0, hi = -1;
    visibleRowRange(lo, hi);
    if (hi < 0)
        return; // nothing laid out yet; the next scroll tick retries
    const int overscan = 20;
    for (int row = qMax(0, lo - overscan); row <= qMin(hi + overscan, m_current.size() - 1);
         ++row) {
        const Post &p = m_current[row];
        const bool persistThumb = MediaCache::persistForPost(p);
        const bool nsfwThumb =
            MediaCache::isNsfwRating(p.rating) ||
            MediaCache::postHasBlacklistedTag(p);
        const QString siteKey = p.isRemote ? p.siteKey : QString();
        if (p.isRemote) {
            const QString thumb = RemotePosts::thumbUrlFor(p);
            if (thumb.isEmpty())
                continue;
            const QString key = QStringLiteral("url|") + thumb;
            if (m_thumbCache.contains(key))
                continue;
            if (isVideoExt(p.ext) && !RemotePosts::isImageUrl(thumb)) {
                // No image preview: decode a frame via the player (slow).
                if (p.fileUrl.isEmpty())
                    continue;
                m_videoThumbs->request(key, p.fileUrl, 0, 0,
                                       m_settings.thumbLongSide, persistThumb,
                                       nsfwThumb, siteKey);
            } else {
                // Image preview (incl. video thumbnails): fast pool decode.
                m_thumbs->request(key, thumb, 0, 0, m_settings.thumbLongSide,
                                  RemotePosts::mediaRefererFor(p), persistThumb,
                                  nsfwThumb, siteKey);
            }
            continue;
        }
        const QFileInfo fi(p.absFile);
        if (!fi.exists())
            continue;
        const qint64 mtime = fi.lastModified().toMSecsSinceEpoch();
        const QString key = p.absFile + u'|' + QString::number(mtime);
        if (m_thumbCache.contains(key))
            continue;
        if (isVideoExt(p.ext))
            m_videoThumbs->request(key, p.absFile, mtime, fi.size(),
                                   m_settings.thumbLongSide, persistThumb,
                                   nsfwThumb, siteKey);
        else
            m_thumbs->request(key, p.absFile, mtime, fi.size(),
                              m_settings.thumbLongSide, QString(), persistThumb,
                              nsfwThumb, siteKey);
    }
    // Pixmap windowing: decoded pixmaps (the actual memory) live only on
    // near-visible rows. Rows scrolled far away drop back to the shared
    // placeholder; scrolling back repaints them from the LRU cache (or
    // re-requests). Model rows themselves stay put, so selection, menus,
    // viewer stepping and the scrollbar never notice the windowing.
    // (Role +16 is the pixmap-loaded flag - +14/+15 are the comment/note
    // pills. They collided once; every thumbnailed card wore a bubble.)
    const int keep = 60;
    for (int row = 0; row < m_model->rowCount(); ++row) {
        const QModelIndex idx = m_model->index(row, 0);
        const bool has = m_model->data(idx, Qt::UserRole + 16).toBool();
        if (row >= lo - keep && row <= hi + keep) {
            if (!has) {
                const auto it = m_thumbCache.find(
                    m_model->data(idx, Qt::UserRole + 4).toString());
                if (it != m_thumbCache.end()) {
                    m_model->setData(idx, *it, Qt::DecorationRole);
                    m_model->setData(idx, true, Qt::UserRole + 16);
                }
            }
        } else if (has) {
            m_model->setData(idx, m_placeholder, Qt::DecorationRole);
            m_model->setData(idx, false, Qt::UserRole + 16);
        }
    }
}

void MainWindow::onThumbnailReady(const QString &key, const QImage &image) {
    QPixmap pm = QPixmap::fromImage(image);
    if (pm.isNull())
        return;
    if (!m_thumbCache.contains(key)) {
        m_cacheOrder.append(key);
        if (m_cacheOrder.size() > 256)
            m_thumbCache.remove(m_cacheOrder.takeFirst());
    }
    m_thumbCache[key] = pm;
    if (m_pages->currentIndex() != 0)
        return; // grid hidden: cache only, paint on return
    // Paint near-visible matches only (indexAt() misses IconMode margins,
    // so each match checks its own rect). Far matches keep the placeholder
    // until scrolled to; the ensure pass in requestVisibleThumbs paints them.
    const int margin = m_grid ? m_grid->rowPitch() * 20 : 0;
    const int vh = m_grid ? m_grid->viewport()->height() : 0;
    for (int row = 0; row < m_model->rowCount(); ++row) {
        const QModelIndex idx = m_model->index(row, 0);
        if (m_model->data(idx, Qt::UserRole + 4).toString() != key)
            continue;
        const QRect ir = m_grid->visualRect(idx);
        if (!ir.isValid() || ir.bottom() < -margin || ir.top() > vh + margin)
            continue;
        m_model->setData(idx, pm, Qt::DecorationRole);
        m_model->setData(idx, true, Qt::UserRole + 16); // pixmap-loaded flag
    }
}

