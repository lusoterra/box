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
#include <QTimer>
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
    // Visible rows first: the pool is FIFO, so queueing the overscan
    // band ahead of the viewport left the cards the user is looking at
    // waiting behind dozens of off-screen downloads (very visible on a
    // slow VPN with full-size Szurubooru previews).
    const int overscan = 10;
    auto requestRow = [&](int row) {
        if (row < 0 || row >= m_current.size())
            return;
        const Post &p = m_current[row];
        // Hydrus has its own storage: never write its thumbnails into the
        // booru media cache. Session-memory previews only.
        const bool persistThumb =
            !isHydrusTab() && MediaCache::persistForPost(p);
        const bool nsfwThumb =
            MediaCache::isNsfwRating(p.rating) ||
            MediaCache::postHasBlacklistedTag(p);
        // Favourites rows carry no stamped key (it is never persisted):
        // resolve through the label so wall-detection can name the site.
        const QString siteKey =
            p.isRemote ? (p.siteKey.isEmpty()
                              ? RemotePosts::keyForLabel(p.siteName)
                              : p.siteKey)
                       : QString();
        if (p.isRemote) {
            const QString thumb = RemotePosts::thumbUrlFor(p);
            if (thumb.isEmpty())
                return;
            const QString key = QStringLiteral("url|") + thumb;
            if (m_thumbCache.contains(key))
                return;
            if (isVideoExt(p.ext) && !RemotePosts::isImageUrl(thumb)) {
                // No image preview: decode a frame via the player (slow).
                if (p.fileUrl.isEmpty())
                    return;
                m_videoThumbs->request(key, p.fileUrl, 0, 0,
                                       m_settings.thumbLongSide, persistThumb,
                                       nsfwThumb, siteKey);
            } else {
                // Image preview (incl. video thumbnails): fast pool decode.
                m_thumbs->request(key, thumb, 0, 0, m_settings.thumbLongSide,
                                  RemotePosts::mediaRefererFor(p), persistThumb,
                                  nsfwThumb, siteKey);
            }
            return;
        }
        const QFileInfo fi(p.absFile);
        if (!fi.exists())
            return;
        const qint64 mtime = fi.lastModified().toMSecsSinceEpoch();
        const QString key = p.absFile + u'|' + QString::number(mtime);
        if (m_thumbCache.contains(key))
            return;
        if (isVideoExt(p.ext))
            m_videoThumbs->request(key, p.absFile, mtime, fi.size(),
                                   m_settings.thumbLongSide, persistThumb,
                                   nsfwThumb, siteKey);
        else
            m_thumbs->request(key, p.absFile, mtime, fi.size(),
                              m_settings.thumbLongSide, QString(), persistThumb,
                              nsfwThumb, siteKey);
    };
    for (int row = lo; row <= hi; ++row)
        requestRow(row);
    for (int row = hi + 1; row <= qMin(hi + overscan, m_current.size() - 1);
         ++row)
        requestRow(row);
    for (int row = lo - 1; row >= qMax(0, lo - overscan); --row)
        requestRow(row);
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
    // The card has pixels now: retire its spinner/progress veil.
    if (m_thumbLoad.remove(key) > 0)
        syncThumbSpinTimer();
    QPixmap pm = QPixmap::fromImage(image);
    if (pm.isNull())
        return;
    // That exact preview no longer needs a Solve button: the wall cleared
    // for it (browser fallback, captured clearance, or a plain retry).
    if (key.startsWith(QStringLiteral("url|"))) {
        const QString url = key.mid(4);
        bool changed = false;
        for (qsizetype i = m_mediaWalls.size() - 1; i >= 0; --i) {
            if (m_mediaWalls.at(i).second == url) {
                m_mediaWalls.removeAt(i);
                changed = true;
            }
        }
        if (changed)
            updateWallButton();
    }
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

void MainWindow::onThumbQueued(const QString &key) {
    if (key.isEmpty() || m_thumbCache.contains(key))
        return;
    // Never downgrade a card that already started moving bytes.
    if (m_thumbLoad.value(key).phase == ThumbLoadState::Downloading)
        return;
    m_thumbLoad.insert(key, ThumbLoadState{ThumbLoadState::Queued, -1});
    updateThumbLoadPaint();
    syncThumbSpinTimer();
}

void MainWindow::onThumbStarted(const QString &key) {
    if (key.isEmpty() || m_thumbCache.contains(key))
        return;
    ThumbLoadState st = m_thumbLoad.value(key);
    st.phase = ThumbLoadState::Downloading;
    m_thumbLoad.insert(key, st);
    updateThumbLoadPaint();
    syncThumbSpinTimer();
}

void MainWindow::onThumbProgress(const QString &key, qint64 received,
                                  qint64 total) {
    if (key.isEmpty() || m_thumbCache.contains(key))
        return;
    ThumbLoadState st;
    st.phase = ThumbLoadState::Downloading;
    st.percent =
        total > 0
            ? int(qBound<qint64>(qint64(0), received * 100 / total, qint64(100)))
            : -1;
    const auto it = m_thumbLoad.constFind(key);
    if (it != m_thumbLoad.constEnd() && it->percent == st.percent &&
        it->phase == st.phase)
        return; // same percent: no repaint needed
    m_thumbLoad.insert(key, st);
    updateThumbLoadPaint();
    syncThumbSpinTimer();
}

void MainWindow::onThumbFailed(const QString &key) {
    if (m_thumbLoad.remove(key) > 0) {
        updateThumbLoadPaint();
        syncThumbSpinTimer();
    }
}

void MainWindow::syncThumbSpinTimer() {
    if (!m_thumbSpinTimer)
        return;
    if (m_thumbLoad.isEmpty()) {
        if (m_thumbSpinTimer->isActive())
            m_thumbSpinTimer->stop();
        if (m_thumbSpinAngle != 0) {
            m_thumbSpinAngle = 0;
            if (m_delegate)
                m_delegate->setSpinAngle(0);
        }
        return;
    }
    if (!m_thumbSpinTimer->isActive())
        m_thumbSpinTimer->start();
}

void MainWindow::updateThumbLoadPaint() {
    if (!m_grid || !m_model || m_thumbLoad.isEmpty())
        return;
    // indexAt() can miss IconMode margins/ragged edges, so use it only
    // to bound the repaint (falling back to the whole viewport) instead
    // of the O(all rows) visibleRowRange() scan on every animation tick.
    const QPoint topLeft(2, 2);
    const QPoint bottomRight(m_grid->viewport()->width() - 3,
                             m_grid->viewport()->height() - 3);
    const QModelIndex first = m_grid->indexAt(topLeft);
    const QModelIndex last = m_grid->indexAt(bottomRight);
    if (!first.isValid() || !last.isValid()) {
        m_grid->viewport()->update();
        return;
    }
    const int lo = qMin(first.row(), last.row()) - 1;
    const int hi = qMax(first.row(), last.row()) + 1;
    for (int row = qMax(0, lo); row <= hi && row < m_model->rowCount();
         ++row) {
        const QModelIndex idx = m_model->index(row, 0);
        const QString key = m_model->data(idx, Qt::UserRole + 4).toString();
        if (!m_thumbLoad.contains(key))
            continue;
        const QRect r = m_grid->visualRect(idx);
        if (r.isValid())
            m_grid->viewport()->update(r);
    }
}

void MainWindow::onThumbWall(const QString &siteKey, const QString &url) {
    // Favourites rows may not carry a key: resolve the media host back to
    // the configured entry (cdn.donmai.us -> danbooru).
    QString key = siteKey;
    if (key.isEmpty() || !RemoteSource::siteFor(key)) {
        if (const BooruSite *entry = RemotePosts::entryForHost(QUrl(url).host()))
            key = entry->key;
    }
    const BooruSite *cfg = RemoteSource::siteFor(key);
    if (!cfg || cfg->baseUrl.isEmpty())
        return; // unknown host: nothing the capture browser could solve
    bool known = false;
    for (auto &wt : m_mediaWalls) {
        if (wt.first == key) {
            wt.second = url;
            known = true;
            break;
        }
    }
    if (!known)
        m_mediaWalls.append({key, url});
    if (!m_wallKinds.contains(key))
        m_wallKinds.insert(key, QStringLiteral("challenge"));
    updateWallButton();
    if (!m_settings.wallAutoPopup)
        return;
    // One capture pass per site, not one per black card: twenty walled
    // thumbnails must summon a single browser. The Solve button stays
    // available for a manual retry later.
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - m_lastWallPopup.value(key, 0) <= 5 * 60 * 1000)
        return;
    m_lastWallPopup.insert(key, now);
    // Root, not the failing CDN URL: the CDN page can load fine (no
    // challenge, no cookies); the clearance comes from the site itself.
    solveWallInBrowser(key, m_wallKinds.value(key));
}

