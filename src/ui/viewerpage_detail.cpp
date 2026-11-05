#include "viewerpage.h"

#include "remotesource.h"

void ViewerPage::maybeFetchHybooruDetail() {
    // HyBooru search summaries carry no tags: backfill them the first
    // time such a post opens, through the normal single-post refresh
    // (tags paint on arrival via onRefreshDone). Session-guarded so
    // genuinely tagless posts never refetch.
    if (m_row < 0 || m_row >= m_posts.size())
        return;
    const Post &p = m_posts.at(m_row);
    if (!p.isRemote || !p.tags.trimmed().isEmpty())
        return;
    const BooruSite *entry = RemotePosts::entryForPost(p);
    if (!entry || entry->engine != BooruEngine::Hybooru)
        return;
    const QString key = entry->key + u'#' + QString::number(p.id);
    if (m_detailFetched.contains(key) || m_detailInFlight.contains(key))
        return;
    // Marked in-flight, not fetched: a failed/cancelled request must retry
    // on the next open (the old code permanently blacklisted the post).
    m_detailInFlight.insert(key);
    emit refreshRequested(p);
}

void ViewerPage::applyRefreshedPost(const Post &post) {
    if (!post.isRemote)
        return;
    const BooruSite *entry = RemotePosts::entryForPost(post);
    const QString key =
        entry ? entry->key + u'#' + QString::number(post.id)
              : (post.siteKey + u'#' + QString::number(post.id));
    m_detailInFlight.remove(key);
    m_detailFetched.insert(key);
    for (int i = 0; i < m_posts.size(); ++i) {
        Post &p = m_posts[i];
        if (!p.isRemote || p.siteName != post.siteName || p.id != post.id)
            continue;
        p = post;
        if (i == m_row)
            renderTags(); // tags/meta only: media must not reload/restart
        break;
    }
}

void ViewerPage::detailFetchFailed(const Post &post) {
    if (!post.isRemote)
        return;
    const BooruSite *entry = RemotePosts::entryForPost(post);
    const QString key =
        entry ? entry->key + u'#' + QString::number(post.id)
              : (post.siteKey + u'#' + QString::number(post.id));
    m_detailInFlight.remove(key);
}

