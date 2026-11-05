#include "viewerpage.h"

#include "flowlayout.h"
#include "imagecache.h"
#include "remotesource.h"
#include "uicons.h"

#include <QBoxLayout>
#include <QCursor>
#include <QDesktopServices>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QSize>
#include <QTimer>
#include <QUrl>

void ViewerPage::rebuildMetaTail() {
    if (!m_tagLayout)
        return;
    if (QLayoutItem *stretch = m_tagLayout->takeAt(m_tagLayout->count() - 1))
        delete stretch;
    while (m_tagLayout->count() > m_metaSectionStart)
        delete m_tagLayout->takeAt(m_metaSectionStart);
    renderMetaSections();
    m_tagLayout->addStretch(1);
}

void ViewerPage::renderMetaSections() {
    m_metaSectionStart = m_tagLayout ? m_tagLayout->count() : 0;    const auto it = m_metaCache.find(metaKey());
    if (it == m_metaCache.end()) {
        if (m_metaFailed.contains(metaKey())) {
            const QString why = m_metaFailureReason.value(metaKey());
            auto *l = new QLabel(this);
            l->setTextFormat(Qt::RichText);
            l->setText(
                why.isEmpty()
                    ? tr("Notes/comments failed to load - <a href=\"#\">click "
                         "for details</a> or reload.")
                    : tr("Notes/comments failed to load (%1) - "
                         "<a href=\"#\">click for details</a>.")
                          .arg(why.toHtmlEscaped()));
            l->setWordWrap(true);
            l->setOpenExternalLinks(false);
            l->setStyleSheet(
                QStringLiteral("color:#e0a055;font-size:11px;"));
            l->setTextInteractionFlags(Qt::TextBrowserInteraction);
            connect(l, &QLabel::linkActivated, this, [this] {
                emit probeRequested(tr("Notes/comments load failed"),
                                    metaProbeText());
            });
            m_tagLayout->addWidget(l);
        }
        return; // still fetching (or failed, noted above)
    }
    const QVector<PostNote> &notes = it->notes;
    const QVector<PostComment> &comments = it->comments;
    const QVector<PostComment> &commentary = it->commentary;
    if (!notes.isEmpty()) {
        auto *hl = new QLabel(tr("NOTES (%1)").arg(notes.size()), this);
        hl->setStyleSheet(QStringLiteral("color:#4a5068;font-size:10px;"));
        m_tagLayout->addWidget(hl);
        for (const PostNote &n : notes) {
            auto *l = new QLabel(
                QStringLiteral("[%1,%2 %3×%4] %5")
                    .arg(n.x)
                    .arg(n.y)
                    .arg(n.w)
                    .arg(n.h)
                    .arg(n.body),
                this);
            l->setWordWrap(true);
            l->setStyleSheet(QStringLiteral("color:#9aa0b8;font-size:11px;"));
            l->setTextInteractionFlags(Qt::TextSelectableByMouse);
            m_tagLayout->addWidget(l);
        }
    }
    if (!commentary.isEmpty()) {
        auto *hl = new QLabel(tr("ARTIST COMMENTARY"), this);
        hl->setStyleSheet(QStringLiteral("color:#4a5068;font-size:10px;"));
        m_tagLayout->addWidget(hl);
        for (const PostComment &c : commentary) {
            auto *l = new QLabel(c.body.toHtmlEscaped(), this);
            l->setWordWrap(true);
            l->setTextFormat(Qt::RichText);
            l->setStyleSheet(QStringLiteral("font-size:11px;"));
            l->setTextInteractionFlags(Qt::TextSelectableByMouse);
            m_tagLayout->addWidget(l);
        }
    }
    if (!comments.isEmpty()) {
        auto *hl = new QLabel(tr("COMMENTS (%1)").arg(comments.size()), this);
        hl->setStyleSheet(QStringLiteral("color:#4a5068;font-size:10px;"));
        m_tagLayout->addWidget(hl);
        // Long threads stay readable: first few show, the rest hide
        // behind a show-all toggle (panel rebuilds reset to collapsed).
        static constexpr int kInitialComments = 5;
        QList<QLabel *> extra;
        int shown = 0;
        for (const PostComment &c : comments) {
            if (++shown > 50)
                break; // hard cap: threads beyond this stay unfetched-light
            const QString head = c.author.isEmpty() ? c.date : c.author + u" · " + c.date;
            auto *l = new QLabel(
                QStringLiteral("<b>%1</b><br>%2")
                    .arg(head.toHtmlEscaped(), c.body.toHtmlEscaped().left(2000)),
                this);
            l->setWordWrap(true);
            l->setStyleSheet(QStringLiteral("font-size:11px;"));
            l->setTextInteractionFlags(Qt::TextSelectableByMouse);
            l->setOpenExternalLinks(false);
            m_tagLayout->addWidget(l);
            if (shown > kInitialComments) {
                l->setVisible(m_commentsExpanded);
                extra.append(l);
            }
        }
        if (!extra.isEmpty()) {
            auto *more = new QPushButton(
                m_commentsExpanded
                    ? tr("Show fewer comments")
                    : tr("Show all %1 comments").arg(comments.size()),
                this);
            more->setFlat(true);
            more->setCursor(Qt::PointingHandCursor);
            more->setStyleSheet(QStringLiteral("color:#7c9ef8;text-align:left;"));
            connect(more, &QPushButton::clicked, this, [this] {
                m_commentsExpanded = !m_commentsExpanded;
                // Deferred: the click handler runs on the toggle itself,
                // which the rebuild deletes.
                QTimer::singleShot(0, this,
                                   [this] { rebuildMetaTail(); });
            });
            m_tagLayout->addWidget(more);
        }
    }
}

void ViewerPage::refetchMeta(const Post &p) {
    if (!p.isRemote || !m_meta)
        return;
    // Entry key first: a custom instance must query its own host,
    // not the preset its engine label maps to.
    const QString key = !p.siteKey.isEmpty()
                            ? p.siteKey
                            : RemotePosts::keyForLabel(p.siteName);
    const BooruSite *site = key.isEmpty() ? nullptr : RemoteSource::siteFor(key);
    if (!site || m_metaCache.contains(metaKey()))
        return;
    // Per-entry credentials (not the hardcoded danbooru/gelbooru keys):
    // renamed or custom instances would otherwise fetch anonymously
    // and fail every load.
    if (site->engine == BooruEngine::Danbooru)
        m_meta->setCreds(site->user, site->secret, {}, {});
    else if (site->engine == BooruEngine::Gelbooru ||
             site->engine == BooruEngine::Safebooru)
        m_meta->setCreds({}, {}, site->user, site->secret);
    else
        m_meta->setCreds({}, {}, {}, {});
    m_meta->fetch(key, site->baseUrl, p.id, site->engine);
}

void ViewerPage::onMetaDone(const QString &site, qint64 postId,
                             QVector<PostNote> notes,
                             QVector<PostComment> comments,
                             QVector<PostComment> commentary, bool ok,
                             const QString &error) {
    const QString key = site + u'#' + QString::number(postId);
    if (!ok) {
        // Failed fetch (network, HTTP error, challenge page): never cache
        // as definitive-empty, and keep any older marks untouched. The
        // thread UI shows the failure with a retry; reopening refetches
        // (the fetch gate only skips cached keys).
        m_metaFailed.insert(key);
        m_metaFailureReason.insert(key, error);
        if (metaKey() == key) {
            if (m_commentsBtn)
                m_commentsBtn->setText(tr("Comments"));
            rebuildMetaTail();
        }
        return;
    }
    m_metaFailed.remove(key);
    m_metaFailureReason.remove(key);
    m_metaCache.insert(key, {notes, comments, commentary});
    // Grid pills learn from the fetched tail too (OR-in downstream):
    // a post whose search row carried no stamp still earns its marks.
    emit metaMarksChanged(site, postId, !comments.isEmpty(), !notes.isEmpty());
    // Re-render only if the user is still on that post (tag host was rebuilt
    // per show, so re-run the tail of showCurrent cheaply: rebuild panel).
    if (metaKey() != site + u'#' + QString::number(postId) || m_tagLayout == nullptr)
        return;
    if (m_commentsBtn) {
        const auto it = m_metaCache.constFind(metaKey());
        const int n = it != m_metaCache.constEnd()
                          ? it->comments.size()
                          : 0;
        m_commentsBtn->setText(n > 0 ? tr("Comments (%1)").arg(n)
                                    : tr("Comments"));
    }
    rebuildMetaTail();
    // Overlay boxes if a still is on screen (applies to the live pixmap).
    stageNotes();
}

void ViewerPage::reloadCurrent() {
    if (m_row < 0 || m_row >= m_posts.size() || m_posts.isEmpty())
        return;
    const Post &p = m_posts[m_row];
    // Tags (+ sidecar) through the normal refresh path.
    emit refreshRequested(p);
    // Comments/notes: drop the cached tail so the fetch below runs again
    // (the open popup, if any, refills itself on the done signal).
    // Key form matters (site key, not display label) or the stale tail
    // survives and the retry silently replays it.
    if (p.isRemote) {
        m_metaCache.remove(metaKey());
        m_metaFailed.remove(metaKey());
        m_metaFailureReason.remove(metaKey());
    }
    // Media bytes: drop the cached file so the re-show refetches.
    if (p.isRemote && m_imgCache) {
        const QString url = RemotePosts::viewerUrlFor(p);
        if (!url.isEmpty())
            m_imgCache->drop(url);
        if (!p.previewUrl.isEmpty() && p.previewUrl != url)
            m_imgCache->drop(p.previewUrl);
    }
    showRow(m_row);
}

// Comment card: avatar circle (initial; no engine exposes photo URLs
// yet - avatarUrl is reserved), bold name · date, selectable body.
QWidget *ViewerPage::commentCard(const PostComment &c, QWidget *parent) {
    auto *card = new QWidget(parent);
    auto *lay = new QHBoxLayout(card);
    lay->setContentsMargins(6, 6, 6, 6);
    lay->setSpacing(8);
    const QString who = c.author.trimmed();
    auto *ava = new QLabel(who.isEmpty() ? QStringLiteral("?") : who.left(1).toUpper(), card);
    ava->setFixedSize(36, 36);
    ava->setAlignment(Qt::AlignCenter);
    // Stable hue per author so threads stay scannable.
    uint h = 0;
    for (const QChar &ch : who)
        h = h * 31 + ch.unicode();
    const QColor bg = QColor::fromHsv(int(h % 256), 110, 110);
    ava->setStyleSheet(QStringLiteral("background:%1;color:white;font-weight:bold;"
                                      "font-size:16px;border-radius:18px;")
                           .arg(bg.name(QColor::HexRgb)));
    lay->addWidget(ava, 0, Qt::AlignTop);
    auto *text = new QWidget(card);
    auto *tlay = new QVBoxLayout(text);
    tlay->setContentsMargins(0, 0, 0, 0);
    tlay->setSpacing(2);
    const QString head = who.isEmpty() ? c.date : who + u" · " + c.date;
    auto *name = new QLabel(QStringLiteral("<b>%1</b>").arg(head.toHtmlEscaped()), text);
    name->setStyleSheet(QStringLiteral("font-size:12px;"));
    name->setTextInteractionFlags(Qt::TextSelectableByMouse);
    tlay->addWidget(name);
    auto *body = new QLabel(c.body.toHtmlEscaped().left(2000), text);
    body->setWordWrap(true);
    body->setStyleSheet(QStringLiteral("font-size:12px;color:#c8cede;"));
    body->setTextInteractionFlags(Qt::TextSelectableByMouse);
    tlay->addWidget(body);
    lay->addWidget(text, 1);
    if (!c.avatarUrl.isEmpty()) {
        // Reserved: no engine provides photo URLs today. When one does,
        // fetch it here and swap the initial out for the photo.
        card->setProperty("avatarUrl", c.avatarUrl);
    }
    return card;
}

void ViewerPage::showCommentsPopup() {
    if (m_row < 0 || m_row >= m_posts.size())
        return;
    const Post &p = m_posts[m_row];
    const QString key = metaKey();
    auto *dlg = new QDialog(this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setWindowTitle(p.isRemote ? tr("Comments - %1 #%2")
                                            .arg(p.siteName)
                                            .arg(p.id)
                                  : tr("Comments"));
    dlg->resize(520, 640);
    auto *lay = new QVBoxLayout(dlg);
    auto *scroll = new QScrollArea(dlg);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto *host = new QWidget(dlg);
    auto *vl = new QVBoxLayout(host);
    vl->setContentsMargins(2, 2, 2, 2);
    auto fill = [this, vl, host, key, p] {
        // Drop the previous content, keep the layout itself.
        QLayoutItem *child;
        while ((child = vl->takeAt(0)) != nullptr) {
            delete child->widget();
            delete child;
        }
        if (key.isEmpty()) {
            auto *l = new QLabel(tr("Comments are only available for "
                                    "online posts."),
                                 host);
            l->setWordWrap(true);
            l->setStyleSheet(QStringLiteral("color:#4a5068;"));
            vl->addWidget(l);
        } else {
            const auto it = m_metaCache.constFind(key);
            if (it == m_metaCache.constEnd() &&
                m_metaFailed.contains(key)) {
                const QString why = m_metaFailureReason.value(key);
                auto *l = new QLabel(host);
                l->setTextFormat(Qt::RichText);
                l->setText(
                    why.isEmpty()
                        ? tr("Couldn't load the thread (network or login "
                             "wall). <a href=\"#\">Details</a>")
                        : tr("Couldn't load the thread (%1). "
                             "<a href=\"#\">Details</a>")
                              .arg(why.toHtmlEscaped()));
                l->setWordWrap(true);
                l->setOpenExternalLinks(false);
                l->setStyleSheet(QStringLiteral("color:#e0a055;"));
                l->setTextInteractionFlags(Qt::TextBrowserInteraction);
                connect(l, &QLabel::linkActivated, this,
                        [this] {
                            emit probeRequested(
                                tr("Notes/comments load failed"),
                                metaProbeText());
                        });
                vl->addWidget(l);
                auto *retry = new QPushButton(tr("Retry"), host);
                connect(retry, &QPushButton::clicked, this,
                        [this, p] { refetchMeta(p); });
                vl->addWidget(retry);
            } else if (it == m_metaCache.constEnd()) {
                auto *l = new QLabel(tr("Loading comments…"), host);
                l->setWordWrap(true);
                l->setStyleSheet(QStringLiteral("color:#4a5068;"));
                vl->addWidget(l);
            } else if (it->comments.isEmpty() && it->commentary.isEmpty()) {
                auto *l = new QLabel(tr("No comments yet."), host);
                l->setWordWrap(true);
                l->setStyleSheet(QStringLiteral("color:#4a5068;"));
                vl->addWidget(l);
            } else {
                if (!it->commentary.isEmpty()) {
                    auto *hl = new QLabel(tr("ARTIST COMMENTARY"), host);
                    hl->setStyleSheet(
                        QStringLiteral("color:#4a5068;font-size:10px;"));
                    vl->addWidget(hl);
                    for (const PostComment &c : it->commentary) {
                        auto *l = new QLabel(c.body.toHtmlEscaped(), host);
                        l->setWordWrap(true);
                        l->setTextFormat(Qt::RichText);
                        l->setTextInteractionFlags(
                            Qt::TextSelectableByMouse);
                        vl->addWidget(l);
                    }
                }
                int shown = 0;
                for (const PostComment &c : it->comments) {
                    if (++shown > 200)
                        break; // threads beyond this stay panel-only
                    vl->addWidget(commentCard(c, host));
                    auto *sep = new QFrame(host);
                    sep->setFrameShape(QFrame::HLine);
                    sep->setStyleSheet(QStringLiteral("color:#2a2f45;"));
                    vl->addWidget(sep);
                }
            }
        }
        vl->addStretch(1);
    };
    fill();
    scroll->setWidget(host);
    lay->addWidget(scroll, 1);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, dlg);
    connect(buttons, &QDialogButtonBox::rejected, dlg, &QDialog::reject);
    lay->addWidget(buttons);
    // A fetch already in flight (triggered when the post opened) lands
    // here: refill once, then go quiet (context-owned: dies with dialog).
    // Failures refill too (error + retry), since nothing was cached.
    connect(m_meta, &PostMetaFetcher::done, dlg,
            [this, dlg, key, fill](const QString &site, qint64 postId,
                                   const QVector<PostNote> &,
                                   const QVector<PostComment> &,
                                   const QVector<PostComment> &, bool,
                                   const QString &) {
                if (key.isEmpty() ||
                    key != site + u'#' + QString::number(postId))
                    return;
                if (m_commentsBtn && metaKey() == key) {
                    const auto it = m_metaCache.constFind(key);
                    const int n = it != m_metaCache.constEnd()
                                      ? it->comments.size()
                                      : 0;
                    m_commentsBtn->setText(n > 0 ? tr("Comments (%1)").arg(n)
                                                : tr("Comments"));
                }
                fill();
            });
    dlg->show();
    dlg->raise();
}

// Queue cached notes (if any) for the current still; they land on the
// pixmap when it arrives (or immediately if already shown).
void ViewerPage::stageNotes() {
    if (m_row < 0 || m_row >= m_posts.size())
        return;
    const Post &p = m_posts[m_row];
    if (isVideoExt(p.ext) || p.ext == QStringLiteral("gif") || !p.width || !p.height) {
        m_image->setNotes({}, {});
        return;
    }
    // Always (re)stage, even when empty: otherwise the previous post's
    // boxes would linger onto an un-noted image.
    const auto it = m_metaCache.find(metaKey());
    if (it == m_metaCache.end())
        m_image->setNotes({}, {});
    else
        m_image->setNotes(it->notes, QSize(p.width, p.height));
}

