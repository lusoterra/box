#include "viewerpage.h"

#include "imagecache.h"
#include "remotesource.h"
#include "uicons.h"
#include "webdlbackend.h"

#include <QAbstractItemModel>

#include <QFileInfo>
#include <QPixmap>
#include <QTimer>
#include <QUrl>

void ViewerPage::step(int d) {
    const int n = m_row + d;
    if (n < 0)
        return;
    if (n >= m_posts.size()) {
        if (d > 0)
            emit pastEnd(); // infinite scroll may have more
        return;
    }
    showRow(n);
}

void ViewerPage::showRow(int row) {
    if (row < 0 || row >= m_posts.size() || m_posts.isEmpty())
        return;
    // Stop previous media + drop any in-flight remote fetch. The player
    // halts on every navigation (not just when the stack shows it): a
    // still's primeThumb can switch the stack away while audio from the
    // previous video keeps going.
    m_video->stop();
    m_gif->clear();
    if (m_imgCache && !m_pendingUrl.isEmpty())
        m_imgCache->cancelUrl(m_pendingUrl);
    m_pendingUrl.clear();
    m_pendingRow = -1;
    m_sampleFallback = false;
    m_webRetried = false;
    dropStillWeb(); // cancel any in-flight browser retry for the old row
    m_loadBar->setVisible(false);
    m_loadInfo->setVisible(false);
    showNote({});
    m_row = row;
    {
        const Post &p = m_posts.at(row);
        const QString drag =
            isVideoExt(p.ext)
                ? QString()
                : (p.isRemote
                       ? RemotePosts::dragUrlFor(p, m_dragLinkMode)
                       : (p.absFile.isEmpty()
                              ? QString()
                              : QUrl::fromLocalFile(p.absFile).toString()));
        m_image->setDragUrl(drag);
        m_gif->setDragUrl(drag);
        m_video->setDragUrl(drag);
    }
    m_commentsExpanded = false; // long threads start collapsed, per post
    emit currentRowChanged(row);
    maybeFetchHybooruDetail();
    primeThumb(row);
    showCurrent();
    preloadNeighbours();
    setFocus();
}

void ViewerPage::primeThumb(int row) {
    // Navigation lands on the target's grid thumbnail instantly; the full
    // file replaces it when ready. Videos skip this (the player surface
    // would just flash over it). GIFs show the thumb inside the gif pane
    // itself, so they never sit on black while the file downloads/decodes.
    if (!m_thumbModel || row < 0)
        return;
    const Post &p = m_posts.at(row);
    if (isVideoExt(p.ext))
        return;
    const QPixmap pm = qvariant_cast<QPixmap>(
        m_thumbModel->index(row, 0).data(Qt::DecorationRole));
    if (pm.isNull())
        return;
    if (p.ext == QStringLiteral("gif")) {
        m_stack->setCurrentWidget(m_gif);
        m_gif->setPlaceholder(pm);
        return;
    }
    m_stack->setCurrentWidget(m_image);
    m_image->setImage(pm.toImage(), false);
}

void ViewerPage::preloadNeighbours() {
    // Warm the two neighbours on each side with the URL the viewer will
    // actually open (sample first: smaller, and exactly what showCurrent
    // requests, so stepping is a cache hit instead of a second fetch).
    // GIFs count too (their samples are small); videos are skipped - a
    // full stream would be huge and a byte range cannot be handed to the
    // player. Their grid thumbnails are already decoded.
    if (!m_imgCache || m_row < 0 || m_row >= m_posts.size())
        return;
    for (int d : {-2, -1, 1, 2}) {
        const int nr = m_row + d;
        if (nr < 0 || nr >= m_posts.size())
            continue;
        const Post &np = m_posts.at(nr);
        if (!np.isRemote)
            continue;
        if (isVideoExt(np.ext)) {
            // Videos cannot be handed to the player from a partial file,
            // but warming the first few MB makes the next open start
            // immediately (DNS/TLS/range cache on the CDN).
            if (!np.fileUrl.isEmpty())
                m_imgCache->prefetchRange(np.fileUrl,
                                          RemotePosts::mediaRefererFor(np));
            continue;
        }
        const QString u = RemotePosts::viewerUrlFor(np);
        if (!u.isEmpty())
            m_imgCache->request(u, RemotePosts::mediaRefererFor(np), &np);
    }
}

void ViewerPage::showCurrent() {
    const Post &p = m_posts[m_row];
    m_posLabel->setText(tr("%1 / %2").arg(m_row + 1).arg(m_posts.size()));
    m_prevBtn->setEnabled(m_row > 0);
    m_nextBtn->setEnabled(m_row + 1 < m_posts.size());
    m_postBtn->setEnabled(!p.postUrl.isEmpty());
    m_postBtn->setToolTip(p.postUrl);
    // Parent/child family: shown only when the API object says one exists
    // (Danbooru/Gelbooru/HyBooru style keys). Some engines use `parent`.
    qint64 parentId = 0;
    for (const char *key : {"parent_id", "parent"}) {
        const QJsonValue v = p.rawJson.value(QLatin1String(key));
        if (!v.isNull() && !v.isUndefined()) {
            parentId = v.toVariant().toLongLong();
            if (parentId > 0)
                break;
        }
    }
    const bool hasChildren =
        p.rawJson.value(QStringLiteral("has_children")).toBool() ||
        p.rawJson.value(QStringLiteral("has_active_children")).toBool() ||
        p.rawJson.value(QStringLiteral("has_visible_children")).toBool();
    m_variantsBtn->setVisible(p.isRemote && p.id > 0 &&
                              (parentId > 0 || hasChildren));
    // Reverse image search needs a fetchable remote file (engines cannot
    // take local paths).
    if (m_imgSearchBtn)
        m_imgSearchBtn->setEnabled(
            p.isRemote && RemotePosts::isHttpUrl(p.fileUrl));
    if (m_commentsBtn) {
        const auto itc = p.isRemote
                             ? m_metaCache.constFind(metaKey())
                             : m_metaCache.constEnd();
        const int n = itc != m_metaCache.constEnd()
                          ? itc->comments.size()
                          : 0;
        m_commentsBtn->setEnabled(p.isRemote);
        m_commentsBtn->setText(n > 0 ? tr("Comments (%1)").arg(n)
                                    : tr("Comments"));
    }

    // Notes + comments for remote posts (cached per session; failures
    // never cache, so reopening refetches).
    if (p.isRemote && m_meta)
        refetchMeta(p);

    // Prerender: the grid thumbnail shows instantly while the full file
    // downloads; replaced below when the real media is ready. GIFs keep
    // the thumb inside the gif pane (never black while loading).
    m_image->setNotes({}, {});
    if (!m_pendingThumb.isNull()) {
        if (p.ext == QStringLiteral("gif")) {
            m_stack->setCurrentWidget(m_gif);
            m_gif->setPlaceholder(m_pendingThumb);
        } else if (!isVideoExt(p.ext)) {
            m_stack->setCurrentWidget(m_image);
            m_image->setImage(m_pendingThumb.toImage(), false);
        }
        m_pendingThumb = QPixmap();
    }

    if (isVideoExt(p.ext)) {
        m_loadBar->setVisible(false);
        m_loadInfo->setVisible(false);
        m_stack->setCurrentWidget(m_video);
        if (p.isRemote) {
            if (p.fileUrl.isEmpty()) {
                // Stale row (login-wall era or pre-file_url): a single-post
                // refresh may restore the URL - once per post per session,
                // then the note below stands.
                const QString key =
                    p.siteName + u'#' + QString::number(p.id);
                if (RemotePosts::isHttpUrl(p.postUrl) &&
                    !m_detailFetched.contains(key)) {
                    m_detailFetched.insert(key);
                    emit refreshRequested(p);
                } else {
                    showNote(tr("No file URL (login may be required)"));
                }
            } else {
                m_video->setUrl(QUrl(p.fileUrl), p);
            }
        } else {
            // Local file: bail early with a clear note if the path is
            // empty or the file is gone.
            if (p.absFile.isEmpty() || !QFileInfo::exists(p.absFile)) {
                m_stack->setCurrentWidget(m_video);
                m_video->stop();
                showNote(tr("Local file not found: %1").arg(p.absFile));
                return;
            }
            m_video->setFile(p.absFile);
        }
    } else if (p.ext == QStringLiteral("gif")) {
        m_loadBar->setVisible(false);
        m_loadInfo->setVisible(false);
        if (p.isRemote) {
            const QString url = RemotePosts::viewerUrlFor(p);
            if (url.isEmpty() || !m_imgCache) {
                showNote(url.isEmpty()
                             ? tr("No file URL (login may be required)")
                             : tr("Image cache unavailable"));
            } else {
                m_stack->setCurrentWidget(m_gif);
                m_pendingUrl = url;
                m_pendingRow = m_row;
                m_loadTimer.restart();
                m_loadLastReceived = 0;
                m_loadLastTime = 0;
                m_loadBar->setRange(0, 0); // busy until progress lands
                m_loadBar->setVisible(true);
                const QString have = m_imgCache->localPath(url, &p);
                if (!have.isEmpty())
                    onCacheReady(url, have);
                else
                    m_imgCache->request(url, RemotePosts::mediaRefererFor(p),
                                        &p);
            }
        } else {
            // Local file: bail early with a clear note if the path is
            // empty or the file is gone (favourites added before download,
            // or files moved outside the app).
            if (p.absFile.isEmpty() || !QFileInfo::exists(p.absFile)) {
                m_stack->setCurrentWidget(m_gif);
                m_gif->clear();
                showNote(tr("Local file not found: %1").arg(p.absFile));
                return;
            }
            m_stack->setCurrentWidget(m_gif);
            m_gif->setFile(p.absFile);
        }
    } else {
        if (p.isRemote) {
            // Full resolution, not the sample: the header shows the real
            // dimensions, so zooming must hold up to them. (GIFs keep the
            // sample path above - full animated GIFs can be enormous.)
            const QString url = !p.fileUrl.isEmpty()
                                    ? p.fileUrl
                                    : RemotePosts::viewerUrlFor(p);
            if (url.isEmpty() || !m_imgCache) {
                m_stack->setCurrentWidget(m_image);
                m_loadInfo->setVisible(false);
                showNote(url.isEmpty()
                             ? tr("No file URL (login may be required)")
                             : tr("Image cache unavailable"));
            } else {
                m_stack->setCurrentWidget(m_image);
                stageNotes();
                m_pendingUrl = url;
                m_pendingRow = m_row;
                m_loadTimer.restart();
                m_loadLastReceived = 0;
                m_loadLastTime = 0;
                m_loadBar->setRange(0, 0); // busy until progress lands
                m_loadBar->setVisible(true);
                const QString have = m_imgCache->localPath(url, &p);
                if (!have.isEmpty())
                    onCacheReady(url, have);
                else
                    m_imgCache->request(url, RemotePosts::mediaRefererFor(p),
                                        &p);
            }
        } else {
            stageNotes();
            loadLocalImage(p.absFile, m_row);
        }
    }

    // Local posts carry a hash id, not a booru number: show nothing.
    m_idLabel->setText(p.isRemote && p.id ? QStringLiteral("#%1").arg(p.id) : QString());
    // Origin: the configured entry's name with the engine family in small
    // ("AIBooru (Danbooru)"), never the bare API label. Browsed posts
    // carry their entry key; favourites and older rows resolve by label.
    if (p.isRemote) {
        const BooruSite *entry = nullptr;
        if (!p.siteKey.isEmpty())
            entry = RemoteSource::siteFor(p.siteKey);
        if (!entry) {
            const QString k = RemotePosts::keyForLabel(p.siteName);
            entry = k.isEmpty() ? nullptr : RemoteSource::siteFor(k);
        }
        if (entry && !entry->label.isEmpty() && entry->label != p.siteName) {
            m_titleLabel->setText(
                QStringLiteral("%1 <span style=\"font-weight:normal;"
                               "font-size:small;\">(%2)</span>")
                    .arg(entry->label.toHtmlEscaped(),
                         RemotePosts::engineLabel(entry->engine)));
        } else {
            m_titleLabel->setText(p.siteName);
        }
    } else if (const BooruSite *entry = RemotePosts::entryForPost(p)) {
        // Downloaded posts keep their booru: same "Label (Engine)"
        // form as remote rows (the artist/title live in the tags).
        if (!entry->label.isEmpty())
            m_titleLabel->setText(
                QStringLiteral("%1 <span style=\"font-weight:normal;"
                               "font-size:small;\">(%2)</span>")
                    .arg(entry->label.toHtmlEscaped(),
                         RemotePosts::engineLabel(entry->engine)));
        else
            m_titleLabel->setText(p.title);
    } else {
        m_titleLabel->setText(p.title);
    }
    m_dateLabel->setText(p.date);
    // The link stays on the post page even after a download links the
    // row to disk; strays (no post URL) keep the local file.
    m_pathLabel->setText(!p.postUrl.isEmpty() ? p.postUrl : p.file);
    m_scoreLabel->setText(p.hasMeta ? QStringLiteral("%1%2")
                                          .arg(p.score > 0 ? QStringLiteral("+")
                                                           : QStringLiteral(""))
                                          .arg(p.score)
                                    : QStringLiteral("-"));
    m_ratingLabel->setText(p.rating +
                           (p.ratingInferred ? tr(" (inferred)") : QString()));
    m_sizeLabel->setText((p.width && p.height)
                             ? QStringLiteral("%1×%2").arg(p.width).arg(p.height)
                             : QStringLiteral("-"));
    m_extLabel->setText(p.ext.toUpper());
    m_srcLabel->setText(p.source.isEmpty()
                            ? QString()
                            : QStringLiteral("<a href=\"%1\">%1</a>")
                                  .arg(p.source.toHtmlEscaped()));

    renderTags();
}


