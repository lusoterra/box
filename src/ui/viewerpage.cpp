#include "viewerpage.h"

#include "imagecache.h"
#include "postmeta.h"
#include "progressbar.h"
#include "remotesource.h"
#include "uicons.h"
#include "webdlbackend.h"

#include <QAbstractItemModel>
#include <QApplication>
#include <QClipboard>
#include <atomic>
#include <QApplication>
#include <QDesktopServices>
#include <QFile>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonValue>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPushButton>
#include <QScrollArea>
#include <QShortcut>
#include <QStyle>
#include <QUrl>
#include <QVBoxLayout>
#include <algorithm>
#include <climits>



// ImageView/GifView/VideoView implementations: viewerpage_mediaviews.cpp

ViewerPage::ViewerPage(QWidget *parent) : QWidget(parent) {
    setFocusPolicy(Qt::StrongFocus);
    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(8, 8, 8, 8);

    // Media widgets first (toolbar buttons connect to m_image below).
    m_stack = new QStackedWidget(this);
    m_image = new ImageView(this);
    m_gif = new GifView(this);
    m_video = new VideoPane(this);
    connect(m_video, &VideoPane::mediaFailed, this,
            &ViewerPage::mediaFailed); // straight relay to the shell
    connect(m_image, &ImageView::fullscreenToggleRequested, this,
            &ViewerPage::fullscreenToggleRequested);
    connect(m_gif, &GifView::fullscreenToggleRequested, this,
            &ViewerPage::fullscreenToggleRequested);
    connect(m_video, &VideoPane::fullscreenToggleRequested, this,
            &ViewerPage::fullscreenToggleRequested);
    auto mediaContext = [this](const QPoint &pos) {
        if (!m_posts.isEmpty() && m_row >= 0 && m_row < m_posts.size())
            emit postContextRequested(m_posts.at(m_row), pos);
    };
    connect(m_image, &ImageView::contextMenuRequested, this, mediaContext);
    connect(m_image, &ImageView::noteActivated, this,
            &ViewerPage::showNotePopup);
    connect(m_gif, &GifView::contextMenuRequested, this, mediaContext);
    connect(m_video, &VideoPane::contextMenuRequested, this, mediaContext);
    connect(m_video, &VideoPane::errorDetailsRequested, this,
            [this](const QString &url, const QString &error) {
                emit probeRequested(
                    tr("Media load failed"),
                    RemotePosts::probeReportText(
                        RemotePosts::redactSecrets(url), 0, error.toUtf8()));
            });
    m_stack->addWidget(m_image);
    m_stack->addWidget(m_gif);
    m_stack->addWidget(m_video);
    m_meta = new PostMetaFetcher(this);
    connect(m_meta, &PostMetaFetcher::done, this, &ViewerPage::onMetaDone);

    auto *bar = new QHBoxLayout();
    auto *backBtn = new QPushButton(QIcon(), tr("Back"), this);
    backBtn->setIcon(UiIcons::icon(QStringLiteral("arrow-left")));
    backBtn->setIconSize(QSize(15, 15));
    m_prevBtn = new QPushButton(this);
    m_prevBtn->setIcon(UiIcons::icon(QStringLiteral("chevron-left")));
    m_prevBtn->setIconSize(QSize(16, 16));
    m_nextBtn = new QPushButton(this);
    m_nextBtn->setIcon(UiIcons::icon(QStringLiteral("chevron-right")));
    m_nextBtn->setIconSize(QSize(16, 16));
    m_posLabel = new QLabel(this);
    m_posLabel->setStyleSheet(QStringLiteral("color:#4a5068;"));
    m_fitBtn = new QPushButton(tr("Fit"), this);
    m_zoomBtn = new QPushButton(tr("100%"), this);
    m_postBtn = new QPushButton(tr("Open post"), this);
    m_postBtn->setIcon(UiIcons::icon(QStringLiteral("external-link")));
    m_postBtn->setIconSize(QSize(15, 15));
    m_variantsBtn = new QPushButton(tr("Variants"), this);
    m_variantsBtn->setToolTip(
        tr("Show this post's parent/child variants in a filtered search"));
    m_variantsBtn->setVisible(false);
    m_imgSearchBtn = new QPushButton(tr("Image search"), this);
    m_imgSearchBtn->setIcon(UiIcons::icon(QStringLiteral("search")));
    m_imgSearchBtn->setIconSize(QSize(15, 15));
    m_imgSearchBtn->setToolTip(
        tr("Reverse image search (SauceNAO, Yandex, Lens, IQDB, trace.moe)"));
    {
        auto *menu = new QMenu(m_imgSearchBtn);
        m_imgSearchBtn->setMenu(menu);
        connect(menu, &QMenu::aboutToShow, this, [this, menu] {
            menu->clear();
            QString fileUrl;
            if (m_row >= 0 && m_row < m_posts.size())
                fileUrl = m_posts.at(m_row).fileUrl;
            for (const auto &[label, url] :
                 RemotePosts::reverseSearchUrls(fileUrl)) {
                QAction *a = menu->addAction(label);
                connect(a, &QAction::triggered, this, [url] {
                    QDesktopServices::openUrl(QUrl(url));
                });
            }
            if (menu->isEmpty()) {
                QAction *none = menu->addAction(tr("No remote file URL"));
                none->setEnabled(false);
            }
        });
    }
    m_downloadBtn = new QPushButton(this);
    m_downloadBtn->setIcon(UiIcons::icon(QStringLiteral("download")));
    m_downloadBtn->setIconSize(QSize(16, 16));
    m_downloadBtn->setFixedWidth(34);
    m_downloadBtn->setToolTip(tr("Save full quality to the download folder"));
    m_refreshBtn = new QPushButton(this);
    m_refreshBtn->setIcon(UiIcons::icon(QStringLiteral("refresh-cw")));
    m_refreshBtn->setIconSize(QSize(16, 16));
    m_refreshBtn->setFixedWidth(34);
    m_refreshBtn->setToolTip(tr("Reload everything (tags, comments, media)"));
    auto *notesBtn = new QPushButton(tr("Notes"), this);
    notesBtn->setCheckable(true);
    notesBtn->setChecked(true);
    notesBtn->setToolTip(tr("Show translation overlays on the image"));
    connect(notesBtn, &QPushButton::toggled, this,
            [this](bool on) { m_image->setNotesVisible(on); });
    m_favBtn = new QPushButton(this);
    m_favBtn->setIcon(UiIcons::heart(QColor(0x78, 0x80, 0xa0), false));
    m_favBtn->setIconSize(QSize(16, 16));
    m_favBtn->setFixedWidth(34);
    m_favBtn->setToolTip(tr("Favourite (F)"));
    auto *openBtn = new QPushButton(this);
    openBtn->setIcon(UiIcons::icon(QStringLiteral("globe")));
    openBtn->setIconSize(QSize(16, 16));
    openBtn->setFixedWidth(34);
    openBtn->setToolTip(tr("Open file / copy URL"));
    auto *fileMenu = new QMenu(openBtn);
    openBtn->setMenu(fileMenu);
    m_commentsBtn = new QPushButton(tr("Comments"), this);
    m_commentsBtn->setToolTip(tr("Show the comment thread in a popup"));
    connect(m_commentsBtn, &QPushButton::clicked, this,
            &ViewerPage::showCommentsPopup);
    connect(backBtn, &QPushButton::clicked, this, &ViewerPage::back);
    connect(m_prevBtn, &QPushButton::clicked, this, [this] { step(-1); });
    connect(m_nextBtn, &QPushButton::clicked, this, [this] { step(1); });
    connect(m_fitBtn, &QPushButton::clicked, this, [this] {
        if (m_stack->currentWidget() == m_gif)
            m_gif->fit();
        else if (m_stack->currentWidget() == m_video)
            m_video->fitView();
        else
            m_image->fit();
    });
    connect(m_zoomBtn, &QPushButton::clicked, this, [this] {
        if (m_stack->currentWidget() == m_gif)
            m_gif->zoom100();
        else if (m_stack->currentWidget() == m_video)
            m_video->zoom100View();
        else
            m_image->zoom100();
    });
    connect(m_variantsBtn, &QPushButton::clicked, this, [this] {
        if (m_row >= 0 && m_row < m_posts.size())
            emit variantsRequested(m_posts[m_row]);
    });
    connect(m_postBtn, &QPushButton::clicked, this, [this] {
        // Scheme-gated: a rogue sidecar/API postUrl like file:///etc
        // must never reach the local file handler on click.
        const QUrl url(m_posts[m_row].postUrl);
        if (!m_posts[m_row].postUrl.isEmpty() &&
            RemotePosts::isHttpUrl(m_posts[m_row].postUrl))
            QDesktopServices::openUrl(url);
    });
    auto *openAct = fileMenu->addAction(tr("Open file"));
    connect(openAct, &QAction::triggered, this, [this] {
        if (m_posts.isEmpty())
            return;
        const Post &p = m_posts[m_row];
        if (p.isRemote && !p.fileUrl.isEmpty()) {
            if (RemotePosts::isHttpUrl(p.fileUrl))
                QDesktopServices::openUrl(QUrl(p.fileUrl));
        } else {
            QDesktopServices::openUrl(QUrl::fromLocalFile(p.absFile));
        }
    });
    auto *copyAct = fileMenu->addAction(tr("Copy file URL"));
    connect(copyAct, &QAction::triggered, this, [this] {
        if (m_posts.isEmpty())
            return;
        const Post &p = m_posts[m_row];
        QApplication::clipboard()->setText(p.isRemote ? p.fileUrl : p.absFile);
    });
    connect(m_downloadBtn, &QPushButton::clicked, this, [this] {
        if (m_posts.isEmpty() || m_row < 0 || m_row >= m_posts.size())
            return;
        if (m_downloaded)
            emit deleteRequested(m_posts[m_row]);
        else
            emit downloadRequested(m_posts[m_row]);
    });
    connect(m_refreshBtn, &QPushButton::clicked, this,
            &ViewerPage::reloadCurrent);
    connect(m_favBtn, &QPushButton::clicked, this, [this] {
        if (!m_posts.isEmpty())
            emit favouriteRequested(m_posts[m_row]);
    });
    auto *favKey = new QShortcut(QKeySequence(Qt::Key_F), this);
    favKey->setContext(Qt::WidgetWithChildrenShortcut);
    connect(favKey, &QShortcut::activated, this, [this] {
        if (!m_posts.isEmpty())
            emit favouriteRequested(m_posts[m_row]);
    });
    bar->addWidget(backBtn);
    bar->addWidget(m_prevBtn);
    bar->addWidget(m_nextBtn);
    bar->addWidget(m_posLabel);
    bar->addWidget(m_fitBtn);
    bar->addWidget(m_zoomBtn);
    m_loadBar = new CenterProgressBar(this);
    m_loadBar->setRange(0, 0); // busy until the first progress lands
    m_loadBar->setFixedWidth(120);
    m_loadBar->setTextVisible(true);
    m_loadBar->setVisible(false);
    bar->addWidget(m_loadBar);

    m_loadInfo = new QLabel(this);
    m_loadInfo->setStyleSheet(QStringLiteral("color:#4a5068;font-size:11px;"));
    m_loadInfo->setVisible(false);
    bar->addWidget(m_loadInfo);
    bar->addStretch(1);
    bar->addWidget(m_variantsBtn);
    bar->addWidget(m_postBtn);
    bar->addWidget(m_imgSearchBtn);
    bar->addWidget(m_refreshBtn);
    bar->addWidget(notesBtn);
    bar->addWidget(m_favBtn);
    bar->addWidget(m_downloadBtn);
    bar->addWidget(openBtn);
    bar->addWidget(m_commentsBtn);
    m_toolbarBox = new QWidget(this);
    m_toolbarBox->setLayout(bar);
    lay->addWidget(m_toolbarBox);

    auto *body = new QHBoxLayout();
    auto *mediaCol = new QWidget(this);
    auto *leftLay = new QVBoxLayout(mediaCol);
    leftLay->setContentsMargins(0, 0, 0, 0);
    leftLay->addWidget(m_stack, 1);
    m_note = new QLabel(this);
    m_note->setStyleSheet(QStringLiteral("color:#7c9ef8;font-size:11px;"));
    m_note->setVisible(false);
    m_note->setOpenExternalLinks(false);
    m_note->setTextInteractionFlags(Qt::TextBrowserInteraction);
    connect(m_note, &QLabel::linkActivated, this, [this](const QString &) {
        if (m_noteUrl.isEmpty())
            return;
        emit probeRequested(
            tr("Media load failed"),
            RemotePosts::probeReportText(
                RemotePosts::redactSecrets(m_noteUrl), 0,
                m_noteError.toUtf8()));
    });
    leftLay->addWidget(m_note);
    body->addWidget(mediaCol, 1);

    auto *panel = new QWidget(this);
    panel->setFixedWidth(290);
    auto *pl = new QVBoxLayout(panel);
    m_idLabel = new QLabel(this);
    m_titleLabel = new QLabel(this);
    m_titleLabel->setWordWrap(true);
    m_titleLabel->setStyleSheet(QStringLiteral("font-weight:bold;"));
    m_dateLabel = new QLabel(this);
    m_pathLabel = new QLabel(this);
    m_pathLabel->setWordWrap(true);
    for (auto *l : {m_idLabel, m_dateLabel, m_pathLabel})
        l->setStyleSheet(QStringLiteral("color:#7880a0;font-size:11px;"));
    pl->addWidget(m_idLabel);
    pl->addWidget(m_titleLabel);
    auto *badgeRow = new QWidget(this);
    auto *badgeLay = new QHBoxLayout(badgeRow);
    badgeLay->setContentsMargins(0, 2, 0, 2);
    badgeLay->setSpacing(4);
    badgeLay->addStretch(0);
    const QString badgeStyle =
        QStringLiteral("color:white;border-radius:4px;padding:1px 7px;"
                       "font-size:10px;background:%1;");
    m_aiBadge = new QLabel(QStringLiteral("AI"), this);
    m_aiBadge->setStyleSheet(
        badgeStyle.arg(QStringLiteral("#7c4dff")));
    m_aiBadge->setToolTip(tr("AI-generated"));
    m_favBadge = new QLabel(QStringLiteral("\u2665"), this);
    m_favBadge->setStyleSheet(badgeStyle.arg(
        UiIcons::favRed().name(QColor::HexRgb)));
    m_favBadge->setToolTip(tr("Favourited"));
    m_dlBadge = new QLabel(QStringLiteral("\u2193"), this);
    m_dlBadge->setStyleSheet(
        badgeStyle.arg(QStringLiteral("#2e8b6e")));
    m_dlBadge->setToolTip(tr("Downloaded"));
    for (auto *b : {m_aiBadge, m_favBadge, m_dlBadge}) {
        b->setVisible(false);
        badgeLay->addWidget(b);
    }
    badgeLay->addStretch(1);
    pl->addWidget(badgeRow);
    pl->addWidget(m_dateLabel);
    pl->addWidget(m_pathLabel);
    auto *grid = new QGridLayout();
    m_scoreLabel = new QLabel(this);
    m_ratingLabel = new QLabel(this);
    m_sizeLabel = new QLabel(this);
    m_extLabel = new QLabel(this);
    const QStringList heads = {tr("Score"), tr("Rating"),
                               tr("Size"), tr("Type")};
    QLabel *vals[] = {m_scoreLabel, m_ratingLabel, m_sizeLabel, m_extLabel};
    for (int i = 0; i < 4; i++) {
        auto *h = new QLabel(heads[i], this);
        h->setStyleSheet(QStringLiteral("color:#4a5068;font-size:9px;"));
        grid->addWidget(h, i / 2 * 2, i % 2);
        grid->addWidget(vals[i], i / 2 * 2 + 1, i % 2);
    }
    pl->addLayout(grid);
    auto *th = new QLabel(tr("TAGS"), this);
    th->setStyleSheet(QStringLiteral("color:#4a5068;font-size:10px;"));
    pl->addWidget(th);
    m_tagScroll = new QScrollArea(this);
    m_tagScroll->setWidgetResizable(true);
    pl->addWidget(m_tagScroll, 1);
    m_srcLabel = new QLabel(this);
    m_srcLabel->setWordWrap(true);
    m_srcLabel->setOpenExternalLinks(true);
    m_srcLabel->setStyleSheet(QStringLiteral("font-size:10px;"));
    pl->addWidget(m_srcLabel);
    m_sidePanel = panel;
    body->addWidget(panel);
    lay->addLayout(body, 1);

    // Esc is owned globally by the main window (viewer -> back, grid ->
    // clear filters). A second Esc shortcut here would make the key
    // ambiguous and fire NEITHER, so only Backspace is local.
    auto *bs = new QShortcut(QKeySequence(Qt::Key_Backspace), this);
    bs->setContext(Qt::WidgetWithChildrenShortcut);
    connect(bs, &QShortcut::activated, this, &ViewerPage::back);
    // Left/Right ALWAYS switch posts, no matter which child has focus
    // (seek slider included - it stays mouse-draggable).
    auto *left = new QShortcut(QKeySequence(Qt::Key_Left), this);
    left->setContext(Qt::WidgetWithChildrenShortcut);
    connect(left, &QShortcut::activated, this, [this] { step(-1); });
    auto *right = new QShortcut(QKeySequence(Qt::Key_Right), this);
    right->setContext(Qt::WidgetWithChildrenShortcut);
    connect(right, &QShortcut::activated, this, [this] { step(1); });
    // Mouse side buttons (Back/Forward) = prev/next picture. Some views
    // swallow mouse presses, so filter the media widgets directly.
    m_image->viewport()->installEventFilter(this);
    m_gif->viewport()->installEventFilter(this);
    m_video->installEventFilter(this);
}

bool ViewerPage::eventFilter(QObject *watched, QEvent *e) {
    if (e->type() == QEvent::MouseButtonPress) {
        const auto button = static_cast<QMouseEvent *>(e)->button();
        if (button == Qt::BackButton) {
            step(-1);
            return true;
        }
        if (button == Qt::ForwardButton) {
            step(1);
            return true;
        }
    }
    return QWidget::eventFilter(watched, e);
}

void ViewerPage::mousePressEvent(QMouseEvent *e) {
    if (e->button() == Qt::BackButton) {
        step(-1);
        e->accept();
    } else if (e->button() == Qt::ForwardButton) {
        step(1);
        e->accept();
    } else {
        QWidget::mousePressEvent(e);
    }
}



void ViewerPage::keyPressEvent(QKeyEvent *e) {
    // Arrows are handled by global shortcuts (always switch posts);
    // keep Esc/Backspace here as a fallback for when shortcuts miss.
    if (e->key() == Qt::Key_Escape || e->key() == Qt::Key_Backspace)
        emit back();
    else
        QWidget::keyPressEvent(e);
}
