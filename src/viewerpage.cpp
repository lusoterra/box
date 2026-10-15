#include "viewerpage.h"

#include "imagecache.h"
#include "postmeta.h"
#include "progressbar.h"
#include "remotesource.h"
#include "uicons.h"

#include <QApplication>
#include <QAbstractItemModel>
#include <QBoxLayout>
#include <QClipboard>
#include <QDesktopServices>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QGraphicsEllipseItem>
#include <QGraphicsRectItem>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QImageReader>
#include <QKeyEvent>
#include <QLabel>
#include <QMediaDevices>
#include <QAudioDevice>
#include <QMouseEvent>
#include <QMovie>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QShortcut>
#include <QStyle>
#include <QStyleOption>
#include <QUrl>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <algorithm>
#include <climits>
#include <QtConcurrent>

#include "flowlayout.h"

// ── ImageView ────────────────────────────────────────────────────

ImageView::ImageView(QWidget *parent) : QGraphicsView(parent) {
    m_scene = new QGraphicsScene(this);
    setScene(m_scene);
    setDragMode(QGraphicsView::ScrollHandDrag);
    setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
    setResizeAnchor(QGraphicsView::AnchorViewCenter);
    setBackgroundBrush(QColor(0x0d, 0x0f, 0x14));
    setFrameShape(QFrame::NoFrame);
}

void ImageView::setImage(const QImage &img, bool applyNotes) {
    m_scene->clear();
    m_item = nullptr;
    if (img.isNull())
        return;
    m_item = m_scene->addPixmap(QPixmap::fromImage(img));
    m_scene->setSceneRect(m_item->boundingRect());
    if (applyNotes)
        applyPendingNotes();
    fit();
}

void ImageView::setNotes(const QVector<PostNote> &notes, const QSize &refSize) {
    m_pendingNotes = notes;
    m_refSize = refSize;
    if (m_item)
        applyPendingNotes();
}

void ImageView::applyPendingNotes() {
    if (!m_item || m_pendingNotes.isEmpty() || !m_refSize.isValid() ||
        m_refSize.width() <= 0 || m_refSize.height() <= 0)
        return;
    const QSizeF disp = m_item->boundingRect().size();
    const double sx = disp.width() / m_refSize.width();
    const double sy = disp.height() / m_refSize.height();
    const QColor accent(0x7c, 0x9e, 0xf8);
    for (const PostNote &n : m_pendingNotes) {
        if (n.w <= 0 || n.h <= 0 || n.body.isEmpty())
            continue;
        auto *box = new QGraphicsRectItem(
            QRectF(n.x * sx, n.y * sy, n.w * sx, n.h * sy), m_item);
        QPen pen(accent);
        pen.setWidth(0); // cosmetic: constant on screen at any zoom
        pen.setJoinStyle(Qt::MiterJoin);
        box->setPen(pen);
        box->setBrush(QColor(accent.red(), accent.green(), accent.blue(), 36));
        box->setToolTip(n.body);
        box->setAcceptHoverEvents(true);
        // Hover dot at the corner (music-player-style affordance).
        auto *dot = new QGraphicsEllipseItem(-3, -3, 6, 6, box);
        dot->setPen(Qt::NoPen);
        dot->setBrush(accent);
        dot->setToolTip(n.body);
    }
    m_pendingNotes.clear();
}

void ImageView::fit() {
    if (!m_item)
        return;
    resetTransform();
    fitInView(m_item, Qt::KeepAspectRatio);
    m_zoom = transform().m11();
    m_fitted = true;
}

void ImageView::zoom100() {
    if (!m_item)
        return;
    resetTransform();
    m_zoom = 1.0;
    m_fitted = false;
    centerOn(m_item);
}

void ImageView::applyZoom(double f, const QPoint &at) {
    if (!m_item)
        return;
    Q_UNUSED(at);
    const double next = qBound(0.05, m_zoom * f, 16.0);
    scale(next / m_zoom, next / m_zoom);
    m_zoom = next;
    m_fitted = false;
}

void ImageView::wheelEvent(QWheelEvent *e) {
    if (!m_item) {
        QGraphicsView::wheelEvent(e);
        return;
    }
    // Free zoom (trackpads steer by pixel sign when there is no angle).
    const int d = e->angleDelta().y() != 0 ? e->angleDelta().y()
                                           : e->pixelDelta().y();
    applyZoom(d > 0 ? 1.15 : 1.0 / 1.15, e->position().toPoint());
    e->accept();
}

bool ImageView::viewportEvent(QEvent *e) {
    // Route viewport wheels exclusively to zoom: otherwise the view both
    // scrolls and zooms on the same tick when zoomed past fit.
    if (e->type() == QEvent::Wheel) {
        wheelEvent(static_cast<QWheelEvent *>(e));
        return true;
    }
    return QGraphicsView::viewportEvent(e);
}

void ImageView::mouseDoubleClickEvent(QMouseEvent *e) {
    if (!m_item)
        return;
    if (m_fitted)
        zoom100();
    else
        fit();
    e->accept();
}

void ImageView::resizeEvent(QResizeEvent *e) {
    QGraphicsView::resizeEvent(e);
    if (m_fitted && m_item)
        fit();
}

// ── GifView ──────────────────────────────────────────────────────

GifView::GifView(QWidget *parent) : QWidget(parent) {
    setFocusPolicy(Qt::StrongFocus);
    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    m_scroll = new QScrollArea(this);
    m_scroll->setWidgetResizable(false);
    m_scroll->setAlignment(Qt::AlignCenter);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_label = new QLabel(this);
    m_label->setAlignment(Qt::AlignCenter);
    m_scroll->setWidget(m_label);
    lay->addWidget(m_scroll);
    m_label->installEventFilter(this);
    // Wheel belongs to zoom only: without this the scroll area scrolls
    // the same tick the zoom handler runs (zoom + scroll fighting).
    m_scroll->viewport()->installEventFilter(this);
}

void GifView::setFile(const QString &path) {
    clear();
    m_movie = new QMovie(path, QByteArray(), this);
    if (!m_movie->isValid()) {
        delete m_movie;
        m_movie = nullptr;
        m_label->setText(tr("Cannot play gif"));
        return;
    }
    // currentPixmap() before start() is 0x0 - and QSize(0,0).isValid() is
    // TRUE, so read the real frame size from the file header instead.
    // (This was the blank-gif bug: base 0x0 -> scaled 0x0 -> 0x0 label.)
    m_sized = false;
    m_base = QSize(320, 240);
    QImageReader probe(path);
    probe.setAutoTransform(true);
    const QSize ps = probe.size();
    if (ps.width() > 0 && ps.height() > 0) {
        m_base = ps;
        m_sized = true;
    }
    connect(m_movie, &QMovie::resized, this, [this](const QSize &) {
        m_label->setFixedSize(m_movie->scaledSize().isValid() ? m_movie->scaledSize()
                                                              : m_base);
    });
    // Late correction in case the header size was wrong: first real frame wins.
    connect(m_movie, &QMovie::frameChanged, this, [this] {
        if (m_sized || !m_movie)
            return;
        const QSize s = m_movie->currentPixmap().size();
        if (s.width() > 0 && s.height() > 0) {
            m_base = s;
            m_sized = true;
            applyZoom();
        }
    });
    m_label->setMovie(m_movie);
    m_zoom = 1.0;
    applyZoom();
    m_movie->start();
}

void GifView::clear() {
    if (m_movie) {
        m_movie->stop();
        delete m_movie;
        m_movie = nullptr;
    }
    m_label->setMovie(nullptr);
    m_label->setText(QString());
}

void GifView::applyZoom() {
    if (!m_movie)
        return;
    m_movie->setScaledSize(m_base * m_zoom);
    m_label->setFixedSize(m_base * m_zoom);
}

void GifView::wheelEvent(QWheelEvent *e) {
    if (!m_movie)
        return;
    const int d = e->angleDelta().y() != 0 ? e->angleDelta().y()
                                           : e->pixelDelta().y();
    // Anchor under cursor: record the content point, zoom, restore it, so
    // repeated ticks don't walk the image away from the pointer.
    QScrollBar *h = m_scroll->horizontalScrollBar();
    QScrollBar *v = m_scroll->verticalScrollBar();
    const QPoint at = e->position().toPoint();
    const QSize old = m_label->size();
    m_zoom = qBound(0.1, m_zoom * (d > 0 ? 1.15 : 1.0 / 1.15), 4.0);
    applyZoom();
    const QSize cur = m_label->size();
    if (old.width() > 0 && old.height() > 0) {
        h->setValue(int((h->value() + at.x()) * double(cur.width()) / old.width()) -
                    at.x());
        v->setValue(int((v->value() + at.y()) * double(cur.height()) / old.height()) -
                    at.y());
    }
    e->accept();
}

bool GifView::eventFilter(QObject *watched, QEvent *e) {
    // Hand-drag panning for the gif label.
    if (watched == m_scroll->viewport() && e->type() == QEvent::Wheel) {
        wheelEvent(static_cast<QWheelEvent *>(e));
        return true;
    }
    if (watched == m_label) {
        if (e->type() == QEvent::MouseButtonPress) {
            auto *me = static_cast<QMouseEvent *>(e);
            if (me->button() == Qt::LeftButton) {
                m_dragging = true;
                m_dragFrom = me->globalPosition().toPoint();
                m_label->setCursor(Qt::ClosedHandCursor);
                return true;
            }
        } else if (e->type() == QEvent::MouseMove) {
            if (m_dragging) {
                auto *me = static_cast<QMouseEvent *>(e);
                const QPoint d = me->globalPosition().toPoint() - m_dragFrom;
                m_dragFrom = me->globalPosition().toPoint();
                m_scroll->horizontalScrollBar()->setValue(
                    m_scroll->horizontalScrollBar()->value() - d.x());
                m_scroll->verticalScrollBar()->setValue(
                    m_scroll->verticalScrollBar()->value() - d.y());
                return true;
            }
        } else if (e->type() == QEvent::MouseButtonRelease) {
            m_dragging = false;
            m_label->unsetCursor();
            return true;
        }
    }
    return QWidget::eventFilter(watched, e);
}

// ── VideoPane ────────────────────────────────────────────────────

void SeekSlider::jumpTo(const QPoint &pos) {
    if (maximum() <= minimum())
        return;
    // Map through the style groove (not the full width): the handle
    // margins would otherwise bias both ends.
    QStyleOptionSlider opt;
    initStyleOption(&opt);
    const QRect groove = style()->subControlRect(QStyle::CC_Slider, &opt,
                                                 QStyle::SC_SliderGroove, this);
    const int span = orientation() == Qt::Horizontal ? groove.width() : groove.height();
    if (span <= 0)
        return;
    const int p = orientation() == Qt::Horizontal ? pos.x() - groove.x()
                                                  : pos.y() - groove.y();
    const int v = QStyle::sliderValueFromPosition(minimum(), maximum(), p, span,
                                                 opt.upsideDown);
    setValue(v);
    emit sliderMoved(v); // live seek (setValue alone doesn't emit this)
}

void SeekSlider::mousePressEvent(QMouseEvent *e) {
    if (e->button() == Qt::LeftButton && orientation() == Qt::Horizontal) {
        m_scrubbing = true;
        setSliderDown(true); // hold player updates while scrubbing
        jumpTo(e->position().toPoint());
        e->accept();
        return;
    }
    QSlider::mousePressEvent(e);
}

void SeekSlider::mouseMoveEvent(QMouseEvent *e) {
    if (m_scrubbing) {
        jumpTo(e->position().toPoint());
        e->accept();
        return;
    }
    QSlider::mouseMoveEvent(e);
}

void SeekSlider::mouseReleaseEvent(QMouseEvent *e) {
    if (m_scrubbing && e->button() == Qt::LeftButton) {
        m_scrubbing = false;
        setSliderDown(false);
        e->accept();
        return;
    }
    QSlider::mouseReleaseEvent(e);
}

VideoPane::VideoPane(QWidget *parent) : QWidget(parent) {
    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    m_video = new QVideoWidget(this);
    m_video->installEventFilter(this); // click toggles pause (below)
    lay->addWidget(m_video, 1);
    m_player = new QMediaPlayer(this);
    m_audio = new QAudioOutput(this);
    m_audio->setDevice(QMediaDevices::defaultAudioOutput());
    m_audio->setVolume(1.0);
    m_audio->setMuted(true); // muted by default; unmute in the controls
    m_player->setAudioOutput(m_audio);
    m_player->setVideoOutput(m_video);
    m_player->setLoops(QMediaPlayer::Infinite);

    auto *ctl = new QHBoxLayout();
    m_playBtn = new QPushButton(this);
    m_playBtn->setIcon(UiIcons::icon(QStringLiteral("pause")));
    m_playBtn->setIconSize(QSize(16, 16));
    m_playBtn->setFixedWidth(36);
    m_slider = new SeekSlider(Qt::Horizontal, this);
    m_time = new QLabel(QStringLiteral("0:00 / 0:00"), this);
    m_time->setStyleSheet(QStringLiteral("color:#4a5068;"));
    m_muteBtn = new QPushButton(this);
    m_muteBtn->setIcon(UiIcons::icon(QStringLiteral("volume-x")));
    m_muteBtn->setIconSize(QSize(16, 16));
    m_muteBtn->setFixedWidth(36);
    m_muteBtn->setToolTip(tr("Mute / unmute"));
    m_vol = new QSlider(Qt::Horizontal, this);
    m_vol->setRange(0, 100);
    m_vol->setValue(100);
    m_vol->setFixedWidth(80);
    m_vol->setToolTip(tr("Volume (system audio device)"));
    m_buf = new QLabel(this);
    m_buf->setStyleSheet(QStringLiteral("color:#7c9ef8;"));
    m_buf->setVisible(false);
    ctl->addWidget(m_playBtn);
    ctl->addWidget(m_slider, 1);
    ctl->addWidget(m_time);
    ctl->addWidget(m_buf);
    ctl->addWidget(m_muteBtn);
    ctl->addWidget(m_vol);
    lay->addLayout(ctl);

    connect(m_player, &QMediaPlayer::mediaStatusChanged, this,
            [this](QMediaPlayer::MediaStatus s) {
        const bool buffering = s == QMediaPlayer::BufferingMedia ||
                               s == QMediaPlayer::StalledMedia ||
                               s == QMediaPlayer::LoadingMedia;
        m_buf->setVisible(buffering);
        if (buffering)
            m_buf->setText(tr("loading…"));
    });

    connect(m_muteBtn, &QPushButton::clicked, this, [this] {
        m_audio->setMuted(!m_audio->isMuted());
    });
    connect(m_audio, &QAudioOutput::mutedChanged, this, [this](bool m) {
        m_muteBtn->setIcon(UiIcons::icon(
            m ? QStringLiteral("volume-x") : QStringLiteral("volume-2")));
    });
    connect(m_vol, &QSlider::valueChanged, this, [this](int v) {
        m_audio->setVolume(v / 100.0);
        if (v > 0 && m_audio->isMuted())
            m_audio->setMuted(false);
    });

    connect(m_playBtn, &QPushButton::clicked, this, [this] { togglePlayPause(); });
    connect(m_player, &QMediaPlayer::playbackStateChanged, this, [this] {
        m_playBtn->setIcon(UiIcons::icon(
            m_player->playbackState() == QMediaPlayer::PlayingState
                ? QStringLiteral("pause")
                : QStringLiteral("play")));
    });
    connect(m_player, &QMediaPlayer::positionChanged, this, [this](qint64 p) {
        if (!m_slider->isSliderDown())
            m_slider->setValue(int(p));
        const qint64 d = m_player->duration();
        m_time->setText(QStringLiteral("%1:%2 / %3:%4")
                            .arg(p / 60000)
                            .arg((p / 1000) % 60, 2, 10, QChar(u'0'))
                            .arg(d / 60000)
                            .arg((d / 1000) % 60, 2, 10, QChar(u'0')));
    });
    connect(m_player, &QMediaPlayer::durationChanged, this,
            [this](qint64 d) { m_slider->setMaximum(int(d)); });
    connect(m_slider, &QSlider::sliderMoved, m_player, &QMediaPlayer::setPosition);
}

void VideoPane::togglePlayPause() {
    if (m_player->playbackState() == QMediaPlayer::PlayingState)
        m_player->pause();
    else
        m_player->play();
}

bool VideoPane::eventFilter(QObject *watched, QEvent *e) {
    // Left-click on the picture toggles pause; anything else (side
    // buttons included) keeps its normal path to the parent viewer.
    if (watched == m_video && e->type() == QEvent::MouseButtonPress &&
        static_cast<QMouseEvent *>(e)->button() == Qt::LeftButton) {
        togglePlayPause();
        return true;
    }
    return QWidget::eventFilter(watched, e);
}

void VideoPane::setDefaults(bool muted, bool loop, int volume) {
    m_muteDefault = muted;
    m_audio->setMuted(muted);
    m_audio->setVolume(qBound(0, volume, 100) / 100.0);
    m_vol->setValue(qBound(0, volume, 100));
    m_player->setLoops(loop ? QMediaPlayer::Infinite : 1);
}

void VideoPane::setFile(const QString &path) {
    setUrl(QUrl::fromLocalFile(path));
}

void VideoPane::setUrl(const QUrl &url) {
    m_audio->setMuted(m_muteDefault); // muted by default, every video
    m_player->setSource(url);
    m_player->play();
}

void VideoPane::stop() {
    m_player->stop();
    m_player->setSource(QUrl());
}

// ── ViewerPage ───────────────────────────────────────────────────

ViewerPage::ViewerPage(QWidget *parent) : QWidget(parent) {
    setFocusPolicy(Qt::StrongFocus);
    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(8, 8, 8, 8);

    // Media widgets first (toolbar buttons connect to m_image below).
    m_stack = new QStackedWidget(this);
    m_image = new ImageView(this);
    m_gif = new GifView(this);
    m_video = new VideoPane(this);
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
    m_downloadBtn = new QPushButton(tr("Download"), this);
    m_downloadBtn->setIcon(UiIcons::icon(QStringLiteral("download")));
    m_downloadBtn->setIconSize(QSize(15, 15));
    m_downloadBtn->setToolTip(tr("Save full quality to the download folder"));
    m_refreshBtn = new QPushButton(tr("Tags"), this);
    m_refreshBtn->setIcon(UiIcons::icon(QStringLiteral("refresh-cw")));
    m_refreshBtn->setIconSize(QSize(15, 15));
    m_refreshBtn->setToolTip(tr("Re-fetch tags from the source booru"));
    m_favBtn = new QPushButton(this);
    m_favBtn->setIcon(UiIcons::heart(QColor(0x78, 0x80, 0xa0), false));
    m_favBtn->setIconSize(QSize(16, 16));
    m_favBtn->setFixedWidth(34);
    m_favBtn->setToolTip(tr("Favourite (F)"));
    auto *openBtn = new QPushButton(tr("Open file"), this);
    auto *copyBtn = new QPushButton(tr("Copy path"), this);
    copyBtn->setIcon(UiIcons::icon(QStringLiteral("copy")));
    copyBtn->setIconSize(QSize(15, 15));
    connect(backBtn, &QPushButton::clicked, this, &ViewerPage::back);
    connect(m_prevBtn, &QPushButton::clicked, this, [this] { step(-1); });
    connect(m_nextBtn, &QPushButton::clicked, this, [this] { step(1); });
    connect(m_fitBtn, &QPushButton::clicked, m_image, &ImageView::fit);
    connect(m_zoomBtn, &QPushButton::clicked, m_image, &ImageView::zoom100);
    connect(m_postBtn, &QPushButton::clicked, this, [this] {
        if (!m_posts[m_row].postUrl.isEmpty())
            QDesktopServices::openUrl(QUrl(m_posts[m_row].postUrl));
    });
    connect(openBtn, &QPushButton::clicked, this, [this] {
        if (m_posts.isEmpty())
            return;
        const Post &p = m_posts[m_row];
        if (p.isRemote && !p.fileUrl.isEmpty())
            QDesktopServices::openUrl(QUrl(p.fileUrl));
        else
            QDesktopServices::openUrl(QUrl::fromLocalFile(p.absFile));
    });
    connect(copyBtn, &QPushButton::clicked, this, [this] {
        if (m_posts.isEmpty())
            return;
        const Post &p = m_posts[m_row];
        QApplication::clipboard()->setText(p.isRemote ? p.fileUrl : p.absFile);
    });
    connect(m_downloadBtn, &QPushButton::clicked, this, [this] {
        if (!m_posts.isEmpty())
            emit downloadRequested(m_posts[m_row]);
    });
    connect(m_refreshBtn, &QPushButton::clicked, this, [this] {
        if (!m_posts.isEmpty())
            emit refreshRequested(m_posts[m_row]);
    });
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
    bar->addStretch(1);
    bar->addWidget(m_postBtn);
    bar->addWidget(m_downloadBtn);
    bar->addWidget(m_refreshBtn);
    bar->addWidget(m_favBtn);
    bar->addWidget(openBtn);
    bar->addWidget(copyBtn);
    lay->addLayout(bar);

    auto *body = new QHBoxLayout();
    auto *mediaCol = new QWidget(this);
    auto *leftLay = new QVBoxLayout(mediaCol);
    leftLay->setContentsMargins(0, 0, 0, 0);
    leftLay->addWidget(m_stack, 1);
    m_note = new QLabel(this);
    m_note->setStyleSheet(QStringLiteral("color:#7c9ef8;font-size:11px;"));
    m_note->setVisible(false);
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
    m_gif->installEventFilter(this);
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

void ViewerPage::setPosts(QVector<Post> posts) {
    m_posts = std::move(posts);
}

void ViewerPage::setPlaybackDefaults(bool muted, bool loop, int volume) {
    m_video->setDefaults(muted, loop, volume);
}

void ViewerPage::setMetaCreds(const QString &danUser, const QString &danKey,
                              const QString &gelUser, const QString &gelKey) {
    if (m_meta)
        m_meta->setCreds(danUser, danKey, gelUser, gelKey);
}

void ViewerPage::setThumbModel(QAbstractItemModel *model) {
    m_thumbModel = model;
}

void ViewerPage::setImageCache(ImageCache *cache) {
    if (m_imgCache)
        disconnect(m_imgCache, nullptr, this, nullptr);
    m_imgCache = cache;
    if (!m_imgCache)
        return;
    connect(m_imgCache, &ImageCache::ready, this, &ViewerPage::onCacheReady);
    connect(m_imgCache, &ImageCache::failed, this, &ViewerPage::onCacheFailed);
    connect(m_imgCache, &ImageCache::downloadProgress, this,
            &ViewerPage::onCacheProgress);
}

void ViewerPage::showNote(const QString &text) {
    m_note->setText(text);
    m_note->setVisible(!text.isEmpty());
}

void ViewerPage::onCacheReady(const QString &url, const QString &localPath) {
    if (url != m_pendingUrl || m_pendingRow != m_row)
        return; // user already navigated away
    m_pendingUrl.clear();
    m_loadBar->setVisible(false);
    showNote({});
    const Post &p = m_posts[m_row];
    if (p.ext == QStringLiteral("gif")) {
        m_stack->setCurrentWidget(m_gif);
        m_gif->setFile(localPath);
    } else {
        loadLocalImage(localPath, m_row);
    }
}

void ViewerPage::onCacheFailed(const QString &url, const QString &error) {
    if (url != m_pendingUrl || m_pendingRow != m_row)
        return;
    m_pendingUrl.clear();
    m_loadBar->setVisible(false);
    showNote(tr("Download failed: %1").arg(error));
}

void ViewerPage::onCacheProgress(const QString &url, qint64 received, qint64 total) {
    if (url != m_pendingUrl || m_pendingRow != m_row || !m_loadBar->isVisible())
        return; // stale (navigated away) or bar repurposed
    if (total > 0) {
        m_loadBar->setRange(0, int(qMin<qint64>(total, INT_MAX)));
        m_loadBar->setValue(int(qMin<qint64>(received, total)));
    } else {
        m_loadBar->setRange(0, 0); // size unknown: busy until it is
    }
}

void ViewerPage::loadLocalImage(const QString &path, int row) {
    m_stack->setCurrentWidget(m_image);
    // Decode off the UI thread. Huge originals are downscaled at decode
    // time (viewerMaxSide setting) - full-res pixmaps stall the view.
    // Local decode has no byte progress: busy spinner until it lands.
    m_loadBar->setRange(0, 0);
    m_loadBar->setVisible(true);
    QFutureWatcher<QImage> *w = new QFutureWatcher<QImage>(this);
    connect(w, &QFutureWatcher<QImage>::finished, this, [this, w, row] {
        m_loadBar->setVisible(false);
        if (row == m_row)
            m_image->setImage(w->result());
        w->deleteLater();
    });
    w->setFuture(QtConcurrent::run([path, cap = m_maxSide] {
        QImageReader reader(path);
        reader.setAutoTransform(true);
        const QSize full = reader.size();
        if (full.isValid() && (full.width() > cap || full.height() > cap)) {
            const double k =
                qMin(double(cap) / full.width(), double(cap) / full.height());
            reader.setScaledSize(
                QSize(qMax(1, int(full.width() * k)), qMax(1, int(full.height() * k))));
        }
        return reader.read();
    }));
}

void ViewerPage::setFaved(bool faved) {
    if (faved)
        m_favBtn->setIcon(UiIcons::heart(UiIcons::favRed(), true));
    else
        m_favBtn->setIcon(UiIcons::heart(QColor(0x78, 0x80, 0xa0), false));
}

void ViewerPage::setRefreshable(bool ok, const QString &reason) {
    m_refreshBtn->setEnabled(ok);
    m_refreshBtn->setToolTip(ok ? tr("Re-fetch tags from the source booru")
                                : reason);
}

void ViewerPage::showThumbPreview(const QPixmap &thumb) {
    m_pendingThumb = thumb;
}

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
    // stop previous media + drop any in-flight remote fetch. The player
    // teardown is skipped unless we are actually leaving the video pane:
    // QMediaPlayer calls are not free, and image->image switches have no
    // pipeline to tear down (stop() runs on every switch otherwise).
    if (m_stack->currentWidget() == m_video)
        m_video->stop();
    m_gif->clear();
    if (m_imgCache && !m_pendingUrl.isEmpty())
        m_imgCache->cancelUrl(m_pendingUrl);
    m_pendingUrl.clear();
    m_pendingRow = -1;
    m_loadBar->setVisible(false);
    showNote({});
    m_row = row;
    primeThumb(row);
    showCurrent();
    preloadNeighbours();
    setFocus();
}

void ViewerPage::primeThumb(int row) {
    // Navigation lands on the target's grid thumbnail instantly; the full
    // file replaces it when ready. Videos skip this (the player surface
    // would just flash over it).
    if (!m_thumbModel || row < 0)
        return;
    const Post &p = m_posts.at(row);
    if (isVideoExt(p.ext))
        return;
    const QPixmap pm = qvariant_cast<QPixmap>(
        m_thumbModel->index(row, 0).data(Qt::DecorationRole));
    if (pm.isNull())
        return;
    m_stack->setCurrentWidget(m_image);
    m_image->setImage(pm.toImage(), false);
}

void ViewerPage::preloadNeighbours() {
    // Full files for the adjacent posts go into the image cache now, so
    // stepping shows them instantly. Stills only: videos stream and full
    // GIFs can be enormous.
    if (!m_imgCache || m_row < 0 || m_row >= m_posts.size())
        return;
    for (int d : {-1, 1}) {
        const int nr = m_row + d;
        if (nr < 0 || nr >= m_posts.size())
            continue;
        const Post &np = m_posts.at(nr);
        if (!np.isRemote || isVideoExt(np.ext) ||
            np.ext == QStringLiteral("gif"))
            continue;
        const QString u = !np.fileUrl.isEmpty()
                              ? np.fileUrl
                              : RemotePosts::viewerUrlFor(np);
        if (!u.isEmpty())
            m_imgCache->request(u, RemotePosts::mediaRefererFor(np));
    }
}

void ViewerPage::showCurrent() {
    const Post &p = m_posts[m_row];
    m_posLabel->setText(tr("%1 / %2").arg(m_row + 1).arg(m_posts.size()));
    m_prevBtn->setEnabled(m_row > 0);
    m_nextBtn->setEnabled(m_row + 1 < m_posts.size());
    m_postBtn->setEnabled(!p.postUrl.isEmpty());
    m_postBtn->setToolTip(p.postUrl);

    // Notes + comments for remote posts (cached per session).
    if (p.isRemote && m_meta) {
        const QString key = RemotePosts::keyForLabel(p.siteName);
        const BooruSite *site = key.isEmpty() ? nullptr : RemoteSource::siteFor(key);
        if (site && !m_metaCache.contains(p.siteName + u'#' + QString::number(p.id)))
            m_meta->fetch(key, site->baseUrl, p.id, site->engine);
    }

    // Prerender: the grid thumbnail shows instantly while the full file
    // downloads; replaced below when the real media is ready.
    m_image->setNotes({}, {});
    if (!m_pendingThumb.isNull()) {
        m_stack->setCurrentWidget(m_image);
        m_image->setImage(m_pendingThumb.toImage(), false);
        m_pendingThumb = QPixmap();
    }

    if (isVideoExt(p.ext)) {
        m_loadBar->setVisible(false);
        m_stack->setCurrentWidget(m_video);
        if (p.isRemote) {
            if (p.fileUrl.isEmpty())
                showNote(tr("No file URL (login may be required)"));
            else
                m_video->setUrl(QUrl(p.fileUrl));
        } else {
            m_video->setFile(p.absFile);
        }
    } else if (p.ext == QStringLiteral("gif")) {
        m_loadBar->setVisible(false);
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
                m_loadBar->setRange(0, 0); // busy until progress lands
                m_loadBar->setVisible(true);
                const QString have = m_imgCache->localPath(url);
                if (!have.isEmpty())
                    onCacheReady(url, have);
                else
                    m_imgCache->request(url, RemotePosts::mediaRefererFor(p));
            }
        } else {
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
                showNote(url.isEmpty()
                             ? tr("No file URL (login may be required)")
                             : tr("Image cache unavailable"));
            } else {
                m_stack->setCurrentWidget(m_image);
                stageNotes();
                m_pendingUrl = url;
                m_pendingRow = m_row;
                m_loadBar->setRange(0, 0); // busy until progress lands
                m_loadBar->setVisible(true);
                const QString have = m_imgCache->localPath(url);
                if (!have.isEmpty())
                    onCacheReady(url, have);
                else
                    m_imgCache->request(url, RemotePosts::mediaRefererFor(p));
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
    } else {
        m_titleLabel->setText(p.title);
    }
    m_dateLabel->setText(p.date);
    m_pathLabel->setText(p.isRemote ? p.postUrl : p.file);
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
        addTagGroup(m_tagLayout, tr("Artist"), p.tagsArtist,
                    QStringLiteral("#d4955a"));
        addTagGroup(m_tagLayout, tr("Copyright"), p.tagsCopyright,
                    QStringLiteral("#b570e0"));
        addTagGroup(m_tagLayout, tr("Character"), p.tagsCharacter,
                    QStringLiteral("#60b880"));
        addTagGroup(m_tagLayout, tr("General"), p.tagsGeneral,
                    QStringLiteral("#7aaee0"));
        addTagGroup(m_tagLayout, tr("Meta"), p.tagsMeta,
                    QStringLiteral("#88aa88"));
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
    return p.siteName + u'#' + QString::number(p.id);
}

void ViewerPage::renderMetaSections() {
    const auto it = m_metaCache.find(metaKey());
    if (it == m_metaCache.end())
        return; // still fetching (or failed silently)
    const auto &[notes, comments] = *it;
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
    if (!comments.isEmpty()) {
        auto *hl = new QLabel(tr("COMMENTS (%1)").arg(comments.size()), this);
        hl->setStyleSheet(QStringLiteral("color:#4a5068;font-size:10px;"));
        m_tagLayout->addWidget(hl);
        int shown = 0;
        for (const PostComment &c : comments) {
            if (++shown > 50)
                break; // long threads stay readable
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
        }
    }
}

void ViewerPage::onMetaDone(const QString &site, qint64 postId,
                            QVector<PostNote> notes, QVector<PostComment> comments) {
    m_metaCache.insert(site + u'#' + QString::number(postId), {notes, comments});
    // Re-render only if the user is still on that post (tag host was rebuilt
    // per show, so re-run the tail of showCurrent cheaply: rebuild panel).
    if (metaKey() != site + u'#' + QString::number(postId) || m_tagLayout == nullptr)
        return;
    // Remove the trailing stretch, append sections, re-add stretch.
    if (QLayoutItem *stretch = m_tagLayout->takeAt(m_tagLayout->count() - 1))
        delete stretch;
    renderMetaSections();
    m_tagLayout->addStretch(1);
    // Overlay boxes if a still is on screen (applies to the live pixmap).
    stageNotes();
}

// Queue cached notes (if any) for the current still; they land on the
// pixmap when it arrives (or immediately if already shown).
void ViewerPage::stageNotes() {
    if (m_row < 0 || m_row >= m_posts.size())
        return;
    const Post &p = m_posts[m_row];
    if (isVideoExt(p.ext) || p.ext == QStringLiteral("gif") || !p.width || !p.height)
        return;
    const auto it = m_metaCache.find(p.isRemote
                                         ? p.siteName + u'#' + QString::number(p.id)
                                         : QString());
    if (it == m_metaCache.end() || it->first.isEmpty())
        return;
    m_image->setNotes(it->first, QSize(p.width, p.height));
}

void ViewerPage::addTagGroup(QBoxLayout *lay, const QString &name, const QString &tags,
                             const QString &color) {
    const QStringList list = tags.split(QChar::Space, Qt::SkipEmptyParts);
    auto *hl = new QLabel(QStringLiteral("%1 (%2)").arg(name).arg(list.size()), this);
    hl->setStyleSheet(QStringLiteral("color:#4a5068;font-size:10px;"));
    lay->addWidget(hl);
    if (list.isEmpty())
        return; // header still shows, with a (0) count
    auto *flow = new FlowLayout(nullptr, 0, 3, 3);
    for (const QString &t : list) {
        QString label = t;
        auto *b = new QPushButton(label.replace(u'_', u' '), this);
        b->setFlat(true);
        b->setCursor(Qt::PointingHandCursor);
        b->setStyleSheet(
            QStringLiteral("text-align:left;color:%1;padding:2px 6px;").arg(color));
        connect(b, &QPushButton::clicked, this, [this, t] { emit tagActivated(t); });
        flow->addWidget(b);
    }
    lay->addLayout(flow);
}

void ViewerPage::keyPressEvent(QKeyEvent *e) {
    // Arrows are handled by global shortcuts (always switch posts);
    // keep Esc/Backspace here as a fallback for when shortcuts miss.
    if (e->key() == Qt::Key_Escape || e->key() == Qt::Key_Backspace)
        emit back();
    else
        QWidget::keyPressEvent(e);
}
