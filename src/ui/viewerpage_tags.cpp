#include "viewerpage.h"

#include "flowlayout.h"
#include "remotesource.h"
#include "uicons.h"

#include <QBoxLayout>
#include <QCursor>
#include <QLabel>
#include <QPushButton>
#include <QSize>
#include <QStringList>
#include <QVBoxLayout>

void ViewerPage::renderTags() {
    if (m_row < 0 || m_row >= m_posts.size())
        return;
    const Post &p = m_posts.at(m_row);

    // Rebuild the tag panel from scratch: destroying the host widget takes
    // all previous tag buttons with it (deleting layouts alone orphans them).
    delete m_tagHost;
    m_tagHost = new QWidget(this);
    m_tagLayout = new QVBoxLayout(m_tagHost);
    m_tagLayout->setContentsMargins(0, 0, 0, 0);
    m_tagScroll->setWidget(m_tagHost);

    if (!p.hasMeta) {
        auto *l = new QLabel(tr("No metadata"), this);
        l->setStyleSheet(QStringLiteral("color:#4a5068;"));
        m_tagLayout->addWidget(l);
    } else {
        const QString siteKey =
            !p.siteKey.isEmpty() ? p.siteKey
                                 : RemotePosts::keyForLabel(p.siteName);
        QMap<QString, QStringList> groups; // classic bucket -> tags
        QMap<QString, QStringList> customGroups;
        QMap<QString, QString> labels; // bucket/key -> display label
        auto appendGroup = [&](const QString &target,
                               const QStringList &tagsIn) {
            if (tagsIn.isEmpty())
                return;
            QStringList tags;
            for (const QString &t : tagsIn) {
                const auto custom =
                    m_customTagCategories.constFind(t.toLower());
                if (custom == m_customTagCategories.constEnd()) {
                    tags << t;
                    continue;
                }
                const QString key = TagCategories::normalizeKey(custom.value());
                customGroups[key] << t;
                labels.insert(key, custom.value());
            }
            if (tags.isEmpty())
                return;
            const QString bucket = TagCategories::classicBucket(target);
            if (!bucket.isEmpty()) {
                groups[bucket] << tags;
                labels.insert(bucket, TagCategories::displayTarget(target));
                return;
            }
            const QString key = TagCategories::targetKey(target);
            if (key.isEmpty())
                return;
            customGroups[key] << tags;
            labels.insert(key, TagCategories::displayTarget(target));
        };
        auto appendBlob = [&](const QString &blob, const QString &bucket) {
            const TagCategoryMerge *m = TagCategories::mergeFor(
                m_categoryMerges, siteKey, bucket);
            appendGroup(m ? m->target : bucket,
                        blob.split(QChar::Space, Qt::SkipEmptyParts));
        };
        appendBlob(p.tagsArtist, QStringLiteral("artist"));
        appendBlob(p.tagsCharacter, QStringLiteral("character"));
        appendBlob(p.tagsCopyright, QStringLiteral("copyright"));
        appendBlob(p.tagsMeta, QStringLiteral("meta"));
        appendBlob(p.tagsGeneral, QStringLiteral("general"));
        for (auto it = p.customTags.begin(); it != p.customTags.end();
             ++it) {
            if (it.value().isEmpty())
                continue;
            const TagCategoryMerge *m = TagCategories::mergeFor(
                m_categoryMerges, siteKey, it.key());
            if (m) {
                appendGroup(m->target, it.value());
                continue;
            }
            const QString key = TagCategories::normalizeKey(it.key());
            customGroups[key] << it.value();
            labels.insert(key, TagCategories::displayTarget(it.key()));
        }
        // Fixed order (Artist, Character, Copyright, General, Meta) even
        // when a merge renames or combines them; native custom categories
        // (weird boorus) follow after.
        const QList<QPair<QString, QString>> order = {
            {QStringLiteral("artist"), tr("Artist")},
            {QStringLiteral("character"), tr("Character")},
            {QStringLiteral("copyright"), tr("Copyright")},
            {QStringLiteral("general"), tr("General")},
            {QStringLiteral("meta"), tr("Meta")},
        };
        for (const auto &[bucket, fallback] : order) {
            const QStringList tags = groups.value(bucket);
            if (tags.isEmpty())
                continue;
            addTagGroup(m_tagLayout, labels.value(bucket, fallback),
                        tags.join(QChar::Space),
                        UiIcons::tagBucketColor(bucket).name());
        }
        for (auto it = customGroups.begin(); it != customGroups.end();
             ++it) {
            addTagGroup(m_tagLayout,
                        labels.value(it.key(),
                                     TagCategories::displayTarget(it.key())),
                        it.value().join(QChar::Space),
                        UiIcons::tagBucketColor(QStringLiteral("custom"))
                            .name());
        }
    }
    if (p.isRemote)
        renderMetaSections();
    m_tagLayout->addStretch(1);
}

QString ViewerPage::metaKey() const {
    if (m_row < 0 || m_row >= m_posts.size())
        return {};
    const Post &p = m_posts[m_row];
    if (!p.isRemote)
        return {};
    // Key form (stamped entry key wins): the fetcher caches under the key
    // it was given, never the display label - lookups must match it or
    // cached comments are invisible and every view refetches.
    const QString key =
        !p.siteKey.isEmpty() ? p.siteKey : RemotePosts::keyForLabel(p.siteName);
    if (key.isEmpty())
        return {};
    return key + u'#' + QString::number(p.id);
}

QString ViewerPage::metaProbeText() const {
    // Post page + failure reason, no URLs/secrets leak (redact first).
    if (m_row < 0 || m_row >= m_posts.size())
        return {};
    const Post &p = m_posts.at(m_row);
    const QString why = m_metaFailureReason.value(metaKey());
    return QStringLiteral("Post: %1\n\n%2")
        .arg(RemotePosts::redactSecrets(p.postUrl.isEmpty() ? p.fileUrl
                                                            : p.postUrl),
             why.isEmpty() ? QStringLiteral("(no reason reported)")
                           : why);
}


void ViewerPage::addTagGroup(QBoxLayout *lay, const QString &name, const QString &tags,
                             const QString &color) {
    const QStringList list = tags.split(QChar::Space, Qt::SkipEmptyParts);
    if (list.isEmpty())
        return; // no header for empty groups (was: `Name (0)`)
    auto *hl = new QLabel(QStringLiteral("%1 (%2)").arg(name).arg(list.size()), this);
    hl->setStyleSheet(QStringLiteral("color:#4a5068;font-size:10px;"));
    lay->addWidget(hl);
    auto *flow = new FlowLayout(nullptr, 0, 3, 3);
    for (const QString &t : list) {
        QString label = t;
        auto *b = new QPushButton(label.replace(u'_', u' '), this);
        b->setFlat(true);
        b->setCursor(Qt::PointingHandCursor);
        b->setStyleSheet(
            QStringLiteral("text-align:left;color:%1;padding:2px 6px;").arg(color));
        connect(b, &QPushButton::clicked, this, [this, t] { emit tagActivated(t); });
        b->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(b, &QPushButton::customContextMenuRequested, this,
                [this, t, b](const QPoint &pos) {
                    emit tagContextRequested(t, b->mapToGlobal(pos));
                });
        flow->addWidget(b);
    }
    lay->addLayout(flow);
}

