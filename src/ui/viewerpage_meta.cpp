#include "viewerpage.h"

#include "flowlayout.h"
#include "imagecache.h"
#include "metatext.h"
#include "remotesource.h"
#include "uicons.h"

#include <QDialogButtonBox>
#include <QLabel>
#include <QPushButton>
#include <QSize>
#include <QTextBrowser>

namespace {
// Markup family + base URL for a post's text fields. Danbooru DText,
// Philomena Markdown, HyBooru plain Hydrus notes; the base resolves their
// relative links.
struct MetaTextContext {
    QUrl base;
    MetaText::Flavor flavor = MetaText::Flavor::DText;
};

MetaTextContext metaTextContextFor(const Post &p) {
    MetaTextContext ctx;
    if (!p.isRemote)
        return ctx;
    const BooruSite *site = RemotePosts::entryForPost(p);
    if (!site)
        return ctx;
    ctx.base = QUrl(site->baseUrl);
    if (RemotePosts::isPhilomenaFamily(site->engine))
        ctx.flavor = MetaText::Flavor::Markdown;
    else if (site->engine == BooruEngine::Hybooru)
        ctx.flavor = MetaText::Flavor::Plain;
    return ctx;
}
} // namespace

void ViewerPage::rebuildMetaTail() {
    if (!m_tagLayout)
        return;
    if (QLayoutItem *stretch = m_tagLayout->takeAt(m_tagLayout->count() - 1))
        delete stretch;
    // takeAt() only detaches: the widgets stay children of the tag host and
    // keep painting at their old geometry, which stacked the expanded
    // comment list on top of the collapsed one. Destroy them with the items.
    while (m_tagLayout->count() > m_metaSectionStart) {
        QLayoutItem *item = m_tagLayout->takeAt(m_metaSectionStart);
        if (QWidget *w = item->widget())
            delete w;
        else if (QLayout *l = item->layout())
            delete l;
        delete item;
    }
    renderMetaSections();
    m_tagLayout->addStretch(1);
}

void ViewerPage::renderMetaSections() {
    m_metaSectionStart = m_tagLayout ? m_tagLayout->count() : 0;
    const auto it = m_metaCache.find(metaKey());
    if (it == m_metaCache.end()) {
        if (m_metaFailed.contains(metaKey())) {
            const QString why = m_metaFailureReason.value(metaKey());
            auto *l = new QLabel(this);
            l->setTextFormat(Qt::RichText);
            l->setText(
                why.isEmpty()
                    ? tr("Post details failed to load - <a href=\"#\">click "
                         "for details</a> or reload.")
                    : tr("Post details failed to load (%1) - "
                         "<a href=\"#\">click for details</a>.")
                          .arg(why.toHtmlEscaped()));
            l->setWordWrap(true);
            l->setOpenExternalLinks(false);
            l->setStyleSheet(
                QStringLiteral("color:#e0a055;font-size:11px;"));
            l->setTextInteractionFlags(Qt::TextBrowserInteraction);
            connect(l, &QLabel::linkActivated, this, [this] {
                emit probeRequested(tr("Post details load failed"),
                                    metaProbeText());
            });
            m_tagLayout->addWidget(l);
        }
        return; // still fetching (or failed, noted above)
    }
    const Post &post =
        (m_row >= 0 && m_row < m_posts.size()) ? m_posts.at(m_row) : Post();
    const MetaTextContext ctx = metaTextContextFor(post);
    const QVector<PostNote> &notes = it->notes;
    const QVector<PostComment> &comments = it->comments;
    const QVector<PostComment> &commentary = it->commentary;
    if (!notes.isEmpty()) {
        auto *hl = new QLabel(tr("NOTES (%1)").arg(notes.size()), this);
        hl->setStyleSheet(QStringLiteral("color:#4a5068;font-size:10px;"));
        m_tagLayout->addWidget(hl);
        int nth = 0;
        for (const PostNote &n : notes) {
            // Bodies are the translation itself: show them, not the box
            // geometry (the coordinates ride the tooltip).
            auto *l = new QLabel(
                QStringLiteral("<b>%1</b> %2")
                    .arg(++nth)
                    .arg(n.body.toHtmlEscaped().left(4000)),
                this);
            l->setTextFormat(Qt::RichText);
            l->setWordWrap(true);
            l->setToolTip(tr("%1,%2 · %3×%4")
                              .arg(n.x)
                              .arg(n.y)
                              .arg(n.w)
                              .arg(n.h));
            l->setStyleSheet(QStringLiteral("color:#9aa0b8;font-size:11px;"));
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
        QList<QWidget *> extra;
        int shown = 0;
        for (const PostComment &c : comments) {
            if (++shown > 50)
                break; // hard cap: threads beyond this stay unfetched-light
            const QString head = c.author.isEmpty() ? c.date : c.author + u" · " + c.date;
            auto *l = new QLabel(
                QStringLiteral("<b>%1</b><br>%2")
                    .arg(head.toHtmlEscaped(),
                         MetaText::toHtml(c.body, ctx.base, 2000,
                                          ctx.flavor)),
                this);
            l->setTextFormat(Qt::RichText);
            l->setWordWrap(true);
            l->setStyleSheet(QStringLiteral("font-size:11px;"));
            l->setTextInteractionFlags(Qt::TextBrowserInteraction);
            l->setOpenExternalLinks(true);
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
            // Toggle the hidden rows in place: a rebuild here re-laid the
            // whole tail and the detached labels ended up stacked on each
            // other (unreadable thread).
            const int total = comments.size();
            connect(more, &QPushButton::clicked, this,
                    [this, more, extra, total] {
                        m_commentsExpanded = !m_commentsExpanded;
                        for (QWidget *w : extra)
                            w->setVisible(m_commentsExpanded);
                        more->setText(m_commentsExpanded
                                          ? tr("Show fewer comments")
                                          : tr("Show all %1 comments")
                                                .arg(total));
                        // Expand just below the button, so the newly shown
                        // rows are in view instead of under the fold.
                        if (m_commentsExpanded && m_tagScroll && m_tagHost)
                            m_tagScroll->ensureWidgetVisible(more);
                    });
            m_tagLayout->addWidget(more);
        }
    }
    if (!commentary.isEmpty()) {
        // Bottom of the tail: a compact clickable preview. Full text (and
        // its links) lives in the popup where it has room to breathe.
        auto *hl = new QLabel(tr("ARTIST COMMENTARY"), this);
        hl->setStyleSheet(QStringLiteral("color:#4a5068;font-size:10px;"));
        m_tagLayout->addWidget(hl);
        QString plain;
        for (const PostComment &c : commentary) {
            const QString t = MetaText::toPlain(c.body, ctx.flavor).simplified();
            if (t.isEmpty())
                continue;
            if (!plain.isEmpty())
                plain += QStringLiteral("  ");
            plain += t;
        }
        static constexpr int kPreviewChars = 280;
        const bool truncated = plain.size() > kPreviewChars;
        if (truncated)
            plain = plain.left(kPreviewChars).trimmed() + QStringLiteral("…");
        if (plain.isEmpty())
            plain = tr("(no text)");
        auto *l = new QLabel(this);
        l->setTextFormat(Qt::RichText);
        l->setWordWrap(true);
        l->setCursor(Qt::PointingHandCursor);
        l->setToolTip(tr("Open the full artist commentary"));
        l->setStyleSheet(QStringLiteral("font-size:11px;"));
        l->setText(
            QStringLiteral("<a href=\"box://commentary\" "
                           "style=\"color:#c8cede;text-decoration:none;\">%1"
                           "<br><span style=\"color:#7c9ef8;font-size:10px;\">"
                           "%2</span></a>")
                .arg(plain.toHtmlEscaped().replace(u'\n',
                                                   QStringLiteral("<br>")),
                     tr("Click to read the full commentary")));
        l->setOpenExternalLinks(false);
        connect(l, &QLabel::linkActivated, this, [this](const QString &href) {
            if (href == QStringLiteral("box://commentary"))
                showCommentaryPopup();
        });
        m_tagLayout->addWidget(l);
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
    const Post p = (m_row >= 0 && m_row < m_posts.size()) ? m_posts.at(m_row)
                                                          : Post();
    const MetaTextContext ctx = metaTextContextFor(p);
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
    auto *body =
        new QLabel(MetaText::toHtml(c.body, ctx.base, 2000, ctx.flavor), text);
    body->setTextFormat(Qt::RichText);
    body->setWordWrap(true);
    body->setStyleSheet(QStringLiteral("font-size:12px;color:#c8cede;"));
    body->setTextInteractionFlags(Qt::TextBrowserInteraction);
    body->setOpenExternalLinks(true);
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
                                tr("Post details load failed"),
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
            } else if (it->comments.isEmpty()) {
                auto *l = new QLabel(tr("No comments yet."), host);
                l->setWordWrap(true);
                l->setStyleSheet(QStringLiteral("color:#4a5068;"));
                vl->addWidget(l);
            } else {
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

void ViewerPage::showCommentaryPopup() {
    if (m_row < 0 || m_row >= m_posts.size())
        return;
    const Post &p = m_posts[m_row];
    const QString key = metaKey();
    const MetaTextContext ctx = metaTextContextFor(p);
    auto *dlg = new QDialog(this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setWindowTitle(p.isRemote ? tr("Artist commentary - %1 #%2")
                                             .arg(p.siteName)
                                             .arg(p.id)
                                   : tr("Artist commentary"));
    dlg->resize(720, 600);
    auto *lay = new QVBoxLayout(dlg);
    auto *browser = new QTextBrowser(dlg);
    browser->setOpenExternalLinks(true);
    browser->setOpenLinks(true);
    browser->setFrameShape(QFrame::NoFrame);
    browser->setStyleSheet(QStringLiteral(
        "QTextBrowser{background:#141824;color:#c8cede;"
        "border:1px solid #2a2f45;border-radius:6px;padding:12px;}"));
    auto fill = [this, browser, key, ctx] {
        const auto it = m_metaCache.constFind(key);
        if (it == m_metaCache.constEnd()) {
            browser->setHtml(
                QStringLiteral("<div style=\"color:#4a5068;\">%1</div>")
                    .arg(tr("Loading artist commentary…").toHtmlEscaped()));
            return;
        }
        if (it->commentary.isEmpty()) {
            browser->setHtml(
                QStringLiteral("<div style=\"color:#4a5068;\">%1</div>")
                    .arg(tr("No artist commentary yet.").toHtmlEscaped()));
            return;
        }
        QString html;
        for (const PostComment &c : it->commentary) {
            if (!html.isEmpty())
                html += QStringLiteral(
                    "<hr style=\"border:none;border-top:1px solid "
                    "#2a2f45;\">");
            html += MetaText::toHtml(c.body, ctx.base, 0, ctx.flavor);
        }
        browser->setHtml(
            QStringLiteral(
                "<div style=\"color:#c8cede;font-size:13px;\">%1</div>")
                .arg(html));
    };
    fill();
    lay->addWidget(browser, 1);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, dlg);
    connect(buttons, &QDialogButtonBox::rejected, dlg, &QDialog::reject);
    lay->addWidget(buttons);
    // A fetch already in flight (or a reload) refills the open dialog.
    if (m_meta) {
        connect(m_meta, &PostMetaFetcher::done, dlg,
                [key, fill](const QString &site, qint64 postId,
                            const QVector<PostNote> &,
                            const QVector<PostComment> &,
                            const QVector<PostComment> &, bool,
                            const QString &) {
                    if (key.isEmpty() ||
                        key != site + u'#' + QString::number(postId))
                        return;
                    fill();
                });
    }
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

