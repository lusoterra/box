#include "viewerpage.h"

#include "imagecache.h"
#include "remotesource.h"
#include "uicons.h"
#include "webdlbackend.h"

#include <QAbstractItemModel>
#include <QLayout>
#include <QPixmap>
#include <QSet>

void ViewerPage::setPosts(QVector<Post> posts) {
    m_posts = std::move(posts);
}

void ViewerPage::setPlaybackDefaults(bool muted, bool loop, int volume) {
    m_video->setDefaults(muted, loop, volume);
}

void ViewerPage::setImmersive(bool on) {
    if (m_toolbarBox)
        m_toolbarBox->setVisible(!on);
    if (m_sidePanel)
        m_sidePanel->setVisible(!on);
    if (auto *l = layout())
        l->setContentsMargins(on ? 0 : 8, on ? 0 : 8, on ? 0 : 8,
                              on ? 0 : 8);
}

void ViewerPage::stopPlayback() {
    if (m_video)
        m_video->stop();
}

void ViewerPage::setMetaCreds(const QString &danUser, const QString &danKey,
                              const QString &gelUser, const QString &gelKey) {
    if (m_meta)
        m_meta->setCreds(danUser, danKey, gelUser, gelKey);
}

void ViewerPage::setThumbModel(QAbstractItemModel *model) {
    m_thumbModel = model;
}

void ViewerPage::setWebBackend(WebDownloadBackend *backend) {
    m_video->setWebBackend(backend);
    if (m_stillWeb == backend)
        return;
    m_stillWeb = backend;
    if (m_stillWeb)
        connect(m_stillWeb, &WebDownloadBackend::finished, this,
                &ViewerPage::onStillWebFinished);
}

void ViewerPage::setCategoryMerges(
    const QList<TagCategoryMerge> &merges) {
    m_categoryMerges = merges;
}

void ViewerPage::setCustomTagCategories(
    const QMap<QString, QString> &tags) {
    m_customTagCategories = tags;
}

void ViewerPage::setImageCache(ImageCache *cache) {
    if (m_imgCache)
        disconnect(m_imgCache, nullptr, this, nullptr);
    m_imgCache = cache;
    if (m_video)
        m_video->setImageCache(cache);
    if (!m_imgCache)
        return;
    connect(m_imgCache, &ImageCache::ready, this, &ViewerPage::onCacheReady);
    connect(m_imgCache, &ImageCache::failed, this, &ViewerPage::onCacheFailed);
    connect(m_imgCache, &ImageCache::downloadProgress, this,
            &ViewerPage::onCacheProgress);
}


bool ViewerPage::hasCurrent() const {
    return m_row >= 0 && m_row < m_posts.size();
}

int ViewerPage::currentRow() const {
    return m_row;
}

void ViewerPage::setStatusBadges(bool ai, bool faved, bool downloaded) {
    if (m_aiBadge)
        m_aiBadge->setVisible(ai);
    if (m_favBadge)
        m_favBadge->setVisible(faved);
    if (m_dlBadge)
        m_dlBadge->setVisible(downloaded);
}

void ViewerPage::setFaved(bool faved) {
    if (faved)
        m_favBtn->setIcon(UiIcons::heart(UiIcons::favRed(), true));
    else
        m_favBtn->setIcon(UiIcons::heart(QColor(0x78, 0x80, 0xa0), false));
}

void ViewerPage::setDownloaded(bool downloaded) {
    m_downloaded = downloaded;
    // Keep the snapshot's cache-policy hint in sync: a post downloaded
    // while its viewer is open becomes cacheable (SFW-only exception) on
    // the next reload/step without rebuilding the grid.
    if (m_row >= 0 && m_row < m_posts.size())
        m_posts[m_row].downloaded = downloaded;
    if (downloaded)
        m_downloadBtn->setIcon(
            UiIcons::icon(QStringLiteral("download"), QColor(0x2e, 0x8b, 0x6e)));
    else
        m_downloadBtn->setIcon(UiIcons::icon(QStringLiteral("download")));
    m_downloadBtn->setToolTip(
        downloaded ? tr("Delete downloaded file from disk")
                   : tr("Save full quality to the download folder"));
}

void ViewerPage::setRefreshable(bool ok, const QString &reason) {
    m_refreshBtn->setEnabled(ok);
    m_refreshBtn->setToolTip(ok ? tr("Re-fetch tags from the source booru")
                                : reason);
}

void ViewerPage::showThumbPreview(const QPixmap &thumb) {
    m_pendingThumb = thumb;
}

QPixmap ViewerPage::currentPixmap() const {
    if (m_stack->currentWidget() == m_image && m_image)
        return m_image->currentPixmap();
    if (m_stack->currentWidget() == m_gif && m_gif)
        return m_gif->currentPixmap();
    return {}; // video, blank, or nothing on screen
}

