#include "viewerpage.h"

#include "imagecache.h"
#include "postmeta.h"
#include "progressbar.h"
#include "remotesource.h"
#include "uicons.h"
#include "webdlbackend.h"

#include <QApplication>
#include <atomic>
#include <QAbstractItemModel>
#include <QBoxLayout>
#include <QClipboard>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QGraphicsEllipseItem>
#include <QGraphicsRectItem>
#include <QVideoFrame>
#include <QVideoSink>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QImageReader>
#include <QKeyEvent>
#include <QLabel>
#include <QMediaDevices>
#include <QAudioDevice>
#include <QMenu>
#include <QMouseEvent>
#include <QMovie>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QShortcut>
#include <QStandardPaths>
#include <QStyle>
#include <QStyleOption>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QElapsedTimer>
#include <algorithm>
#include <climits>
#include <QtConcurrent>

#include "flowlayout.h"

namespace {
QString formatBytes(qint64 bytes) {
    if (bytes < 1024)
        return QString::number(bytes) + QStringLiteral(" B");
    if (bytes < 1024 * 1024)
        return QString::number(bytes / 1024.0, 'f', 1) + QStringLiteral(" KB");
    return QString::number(bytes / (1024.0 * 1024.0), 'f', 1) + QStringLiteral(" MB");
}
} // namespace

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
    m_noteBoxes.clear(); // scene clear deleted them; list must not dangle
    m_item = nullptr;
    if (img.isNull())
        return;
    m_item = m_scene->addPixmap(QPixmap::fromImage(img));
    m_scene->setSceneRect(m_item->boundingRect());
    if (applyNotes)
        applyPendingNotes();
    fit();
}

QPixmap ImageView::currentPixmap() const {
    return m_item ? m_item->pixmap() : QPixmap();
}

void ImageView::setNotes(const QVector<PostNote> &notes, const QSize &refSize) {
    m_notes = notes;
    m_refSize = refSize;
    if (m_item)
        applyPendingNotes();
}

void ImageView::setNotesVisible(bool visible) {
    if (m_notesVisible == visible)
        return;
    m_notesVisible = visible;
    if (visible) {
        applyPendingNotes(); // rebuilds from the stored notes
        return;
    }
    for (QGraphicsRectItem *box : m_noteBoxes)
        box->setVisible(false);
}

void ImageView::applyPendingNotes() {
    qDeleteAll(m_noteBoxes);
    m_noteBoxes.clear();
    if (!m_item || !m_notesVisible || m_notes.isEmpty() ||
        !m_refSize.isValid() || m_refSize.width() <= 0 ||
        m_refSize.height() <= 0)
        return;
    const QSizeF disp = m_item->boundingRect().size();
    const double sx = disp.width() / m_refSize.width();
    const double sy = disp.height() / m_refSize.height();
    const QColor accent(0x7c, 0x9e, 0xf8);
    for (const PostNote &n : m_notes) {
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
        m_noteBoxes.append(box);
    }
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

GifView::GifView(QWidget *parent) : QGraphicsView(parent) {
    m_scene = new QGraphicsScene(this);
    setScene(m_scene);
    setDragMode(QGraphicsView::ScrollHandDrag);
    setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
    setResizeAnchor(QGraphicsView::AnchorViewCenter);
    setBackgroundBrush(QColor(0x0d, 0x0f, 0x14));
    setFrameShape(QFrame::NoFrame);
}

void GifView::setFile(const QString &path) {
    // Keep any placeholder thumb on screen: tearing it down here is what
    // left GIFs on black while the first frame decoded. The frameChanged
    // handler below swaps the placeholder pixmap in place when frame 0
    // lands (same-size fast path, re-fit when the size changes).
    if (m_movie) {
        m_movie->stop();
        delete m_movie;
        m_movie = nullptr;
    }
    m_movie = new QMovie(path, QByteArray(), this);
    if (!m_movie->isValid()) {
        delete m_movie;
        m_movie = nullptr;
        m_scene->clear();
        m_item = nullptr;
        auto *text = m_scene->addText(tr("Cannot play gif"));
        text->setDefaultTextColor(QColor(0xe0, 0x55, 0x55));
        Q_UNUSED(text);
        return; // nothing to render: leave the scene empty
    }
    // First real frame sizes the scene (header probes misreported 0x0
    // before — this was the blank-gif bug); every frame after that just
    // swaps the pixmap, so zoom never touches widget geometry.
    // Animated frames can report varying sizes (placeholders, disposal
    // rects): when the size changes, refresh the scene rect and re-apply
    // the current fit/zoom so scrollbars and the under-mouse anchor don't
    // fight the incoming frame (the still/gif zoom-flicker bug).
    // A single-frame "gif" is not animated: show it as a still instead of
    // a frozen movie (QMovie reports frameCount() == 1 for those).
    connect(m_movie, &QMovie::frameChanged, this, [this] {
        if (!m_movie)
            return;
        const QPixmap pm = m_movie->currentPixmap();
        if (pm.isNull())
            return;
        if (!m_item) {
            m_item = m_scene->addPixmap(pm);
            m_scene->setSceneRect(m_item->boundingRect());
            fit();
        } else {
            if (pm.size() != m_item->pixmap().size()) {
                m_item->setPixmap(pm);
                m_scene->setSceneRect(m_item->boundingRect());
                if (m_fitted) {
                    fit();
                } else {
                    const double z = m_zoom;
                    resetTransform();
                    scale(z, z);
                    m_zoom = z;
                }
            } else {
                m_item->setPixmap(pm);
            }
        }
    });
    if (m_movie->frameCount() <= 1) {
        // Not actually animated: render the one frame as a still and drop
        // the movie so there is no dead animation object lingering. Jump to
        // frame 0 to force a decode; the connected frameChanged handler adds
        // the pixmap item (and fits) just like the first frame of an anim.
        m_movie->jumpToFrame(0);
        delete m_movie;
        m_movie = nullptr;
        return;
    }
    m_movie->start();
}

void GifView::setPlaceholder(const QPixmap &pm) {
    clear();
    if (pm.isNull())
        return;
    m_item = m_scene->addPixmap(pm);
    m_scene->setSceneRect(m_item->boundingRect());
    fit();
}

QPixmap GifView::currentPixmap() const {
    return m_item ? m_item->pixmap() : QPixmap();
}

void GifView::clear() {
    if (m_movie) {
        m_movie->stop();
        delete m_movie;
        m_movie = nullptr;
    }
    m_scene->clear();
    m_item = nullptr;
}

void GifView::fit() {
    if (!m_item)
        return;
    resetTransform();
    fitInView(m_item, Qt::KeepAspectRatio);
    m_zoom = transform().m11();
    m_fitted = true;
}

void GifView::zoom100() {
    if (!m_item)
        return;
    resetTransform();
    m_zoom = 1.0;
    m_fitted = false;
    centerOn(m_item);
}

void GifView::applyZoom(double f) {
    if (!m_item)
        return;
    const double next = qBound(0.05, m_zoom * f, 16.0);
    scale(next / m_zoom, next / m_zoom);
    m_zoom = next;
    m_fitted = false;
}

void GifView::wheelEvent(QWheelEvent *e) {
    if (!m_item) {
        QGraphicsView::wheelEvent(e);
        return;
    }
    // Free zoom (trackpads steer by pixel sign when there is no angle).
    const int d = e->angleDelta().y() != 0 ? e->angleDelta().y()
                                           : e->pixelDelta().y();
    applyZoom(d > 0 ? 1.15 : 1.0 / 1.15);
    e->accept();
}

bool GifView::viewportEvent(QEvent *e) {
    // Route viewport wheels exclusively to zoom: otherwise the view both
    // scrolls and zooms on the same tick when zoomed past fit.
    if (e->type() == QEvent::Wheel) {
        wheelEvent(static_cast<QWheelEvent *>(e));
        return true;
    }
    return QGraphicsView::viewportEvent(e);
}

void GifView::mouseDoubleClickEvent(QMouseEvent *e) {
    if (!m_item)
        return;
    if (m_fitted)
        zoom100();
    else
        fit();
    e->accept();
}

void GifView::resizeEvent(QResizeEvent *e) {
    QGraphicsView::resizeEvent(e);
    if (m_fitted && m_item)
        fit();
}

// ── VideoView ────────────────────────────────────────────────────

VideoFrameItem::VideoFrameItem(QGraphicsItem *parent) : QGraphicsItem(parent) {
}

void VideoFrameItem::setFrame(const QVideoFrame &frame) {
    QImage img;
    bool portrait = false;
    if (frame.isValid()) {
        QVideoFrame f(frame); // shared copy; safe past the signal
        img = f.toImage();
        // Mappable-handle fallback only: map() on a non-readable frame
        // (broken hardware surfaces) warns `Cannot map ... ReadOnly`
        // and has preceded a SIGSEGV in the backend — never attempt it.
        if (img.isNull() && f.isReadable() && f.map(QVideoFrame::ReadOnly)) {
            img = f.toImage(); // mapped-state path for exotic handles
            f.unmap();
        }
        img = img.copy(); // detach: the frame dies with the signal
        // Phone clips arrive sideways with a rotation flag (the old
        // video item applied this itself); swap to display size.
        const QtVideo::Rotation rot = f.surfaceFormat().rotation();
        portrait = rot == QtVideo::Rotation::Clockwise90 ||
                   rot == QtVideo::Rotation::Clockwise270;
    }
    const QSizeF display =
        img.isNull()
            ? QSizeF()
            : portrait ? QSizeF(img.height(), img.width())
                       : QSizeF(img.size());
    if (display != m_size) {
        prepareGeometryChange();
        m_size = display;
    }
    m_image = img;
    update();
}

QRectF VideoFrameItem::boundingRect() const {
    return QRectF(QPointF(), m_size);
}

void VideoFrameItem::paint(QPainter *painter,
                           const QStyleOptionGraphicsItem *,
                           QWidget *) {
    if (m_image.isNull())
        return;
    painter->setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter->drawImage(boundingRect(), m_image);
}

VideoView::VideoView(QWidget *parent) : QGraphicsView(parent) {
    m_scene = new QGraphicsScene(this);
    setScene(m_scene);
    setDragMode(QGraphicsView::ScrollHandDrag);
    setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
    setResizeAnchor(QGraphicsView::AnchorViewCenter);
    setBackgroundBrush(QColor(0x0d, 0x0f, 0x14));
    setFrameShape(QFrame::NoFrame);
    m_item = new VideoFrameItem();
    m_scene->addItem(m_item);
    m_sink = new QVideoSink(this);
    // Frames carry their own dimensions, so the scene tracks whatever
    // actually renders: no metadata/native-size event required. The fit
    // only re-applies while fitted or on a size change, so zoom never
    // fights the incoming frames.
    connect(m_sink, &QVideoSink::videoFrameChanged, this,
            [this](const QVideoFrame &frame) {
                m_item->setFrame(frame);
                const QRectF b = m_item->boundingRect();
                if (b.isEmpty())
                    return;
                if (m_scene->sceneRect() != b) {
                    m_scene->setSceneRect(b);
                    if (m_fitted)
                        fit();
                }
            });
}

bool VideoView::hasFrames() const {
    return !m_scene->sceneRect().isEmpty();
}

void VideoView::resetSize() {
    if (m_item)
        m_item->setFrame(QVideoFrame());
    m_scene->setSceneRect(QRectF());
    resetTransform();
    m_zoom = 1.0;
    m_fitted = true;
}

void VideoView::fit() {
    resetTransform();
    // Empty scene (no size yet): fall back to the item's own rect when
    // it has one, so fit degrades to a no-op only while truly sizeless.
    QRectF r = m_scene->sceneRect();
    if (r.isEmpty() && m_item && !m_item->boundingRect().isEmpty()) {
        r = m_item->boundingRect();
        m_scene->setSceneRect(r);
    }
    if (!r.isEmpty())
        fitInView(r, Qt::KeepAspectRatio);
    m_zoom = transform().m11();
    if (m_zoom <= 0)
        m_zoom = 1.0;
    m_fitted = true;
}

void VideoView::zoom100() {
    resetTransform();
    m_zoom = 1.0;
    m_fitted = false;
    centerOn(m_scene->sceneRect().center());
}

void VideoView::applyZoom(double f) {
    const double next = qBound(0.05, m_zoom * f, 16.0);
    scale(next / m_zoom, next / m_zoom);
    m_zoom = next;
    m_fitted = false;
}

void VideoView::wheelEvent(QWheelEvent *e) {
    // Free zoom (trackpads steer by pixel sign when there is no angle).
    const int d = e->angleDelta().y() != 0 ? e->angleDelta().y()
                                           : e->pixelDelta().y();
    applyZoom(d > 0 ? 1.15 : 1.0 / 1.15);
    e->accept();
}

bool VideoView::viewportEvent(QEvent *e) {
    // Route viewport wheels exclusively to zoom: otherwise the view both
    // scrolls and zooms on the same tick when zoomed past fit.
    if (e->type() == QEvent::Wheel) {
        wheelEvent(static_cast<QWheelEvent *>(e));
        return true;
    }
    return QGraphicsView::viewportEvent(e);
}

void VideoView::mouseDoubleClickEvent(QMouseEvent *e) {
    if (m_fitted)
        zoom100();
    else
        fit();
    e->accept();
}

void VideoView::resizeEvent(QResizeEvent *e) {
    QGraphicsView::resizeEvent(e);
    if (m_fitted)
        fit();
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
    m_view = new VideoView(this);
    m_view->viewport()->installEventFilter(this); // click toggles pause
    lay->addWidget(m_view, 1);
    m_player = new QMediaPlayer(this);
    m_audio = new QAudioOutput(this);
    m_audio->setDevice(QMediaDevices::defaultAudioOutput());
    m_audio->setVolume(1.0);
    m_audio->setMuted(true); // muted by default; unmute in the controls
    m_player->setAudioOutput(m_audio);
    m_player->setVideoOutput(m_view->videoSink());
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
    m_rateBtn = new QPushButton(QStringLiteral("1×"), this);
    m_rateBtn->setToolTip(tr("Playback speed"));
    m_rateBtn->setFixedWidth(44);
    {
        auto *rateMenu = new QMenu(m_rateBtn);
        m_rateBtn->setMenu(rateMenu);
        connect(rateMenu, &QMenu::aboutToShow, this, [this, rateMenu] {
            rateMenu->clear();
            for (const double r : {0.25, 0.5, 1.0, 1.5, 2.0}) {
                QAction *a = rateMenu->addAction(
                    QStringLiteral("%1×").arg(r));
                a->setCheckable(true);
                a->setChecked(qFuzzyCompare(r + 1.0, m_rate + 1.0));
                connect(a, &QAction::triggered, this,
                        [this, r] { setRate(r); });
            }
        });
    }
    m_buf = new QLabel(this);
    m_buf->setStyleSheet(QStringLiteral("color:#7c9ef8;"));
    m_buf->setVisible(false);
    ctl->addWidget(m_playBtn);
    ctl->addWidget(m_slider, 1);
    ctl->addWidget(m_time);
    ctl->addWidget(m_buf);
    ctl->addWidget(m_muteBtn);
    ctl->addWidget(m_vol);
    ctl->addWidget(m_rateBtn);
    lay->addLayout(ctl);

    connect(m_player, &QMediaPlayer::mediaStatusChanged, this,
            [this](QMediaPlayer::MediaStatus s) {
        const bool buffering = s == QMediaPlayer::BufferingMedia ||
                               s == QMediaPlayer::StalledMedia ||
                               s == QMediaPlayer::LoadingMedia;
        // A deferred red error owns the label once shown; transient
        // loading ticks must not flash over it (or paint red too early).
        if (!m_errorShown) {
            m_buf->setVisible(buffering);
            if (buffering)
                m_buf->setText(tr("loading…"));
        }
        if (s == QMediaPlayer::LoadedMedia) {
            m_player->setPlaybackRate(m_rate); // stick across videos in-session
            m_player->play(); // setSource() is async: play once the source is ready
        }
        if (s == QMediaPlayer::InvalidMedia)
            onPlayerError(tr("Cannot play this video"));
        // EndOfMedia with an active error surfaces via errorOccurred;
        // a clean loop/end needs no error text.
    });
    connect(m_player, &QMediaPlayer::errorOccurred, this,
            [this](QMediaPlayer::Error error, const QString &errorString) {
        if (error == QMediaPlayer::NoError)
            return;
        // Transient backend complaints during start-up/stalls cry wolf
        // while the stream may still recover: only terminal statuses
        // (InvalidMedia / EndOfMedia with an error) may paint red.
        // The matching InvalidMedia tick re-enters here if it is real.
        const QMediaPlayer::MediaStatus s = m_player->mediaStatus();
        if (s == QMediaPlayer::LoadingMedia ||
            s == QMediaPlayer::BufferingMedia ||
            s == QMediaPlayer::StalledMedia)
            return;
        onPlayerError(errorString);
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
        // The direct stream recovered after an error (or a fallback
        // it no longer needs): frames are rendering, so drop the
        // stale error and stop the redundant buffered fetch.
        if (m_errorShown && m_view && m_view->hasFrames()) {
            m_errorShown = false;
            m_buf->setVisible(false);
            if (m_imgCache && !m_cacheUrl.isEmpty()) {
                m_imgCache->cancelUrl(m_cacheUrl);
                m_cacheUrl.clear();
            }
            m_fallbackTried = false;
        }
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

void VideoPane::fitView() {
    if (m_view)
        m_view->fit();
}

void VideoPane::zoom100View() {
    if (m_view)
        m_view->zoom100();
}

bool VideoPane::eventFilter(QObject *watched, QEvent *e) {
    // Left-click on the picture toggles pause; anything else (side
    // buttons included) keeps its normal path to the parent viewer.
    // NOTE: drag-pan starts with a left press too — the click only
    // toggles when the pointer never moved (i.e. not a pan), and only
    // then is the release consumed so pans always finish cleanly.
    if (watched == m_view->viewport() &&
        e->type() == QEvent::MouseButtonPress &&
        static_cast<QMouseEvent *>(e)->button() == Qt::LeftButton) {
        m_pressPos = static_cast<QMouseEvent *>(e)->position().toPoint();
        m_pressed = true;
        return false; // let ScrollHandDrag see it too
    }
    if (watched == m_view->viewport() &&
        e->type() == QEvent::MouseButtonRelease &&
        static_cast<QMouseEvent *>(e)->button() == Qt::LeftButton) {
        const bool click = m_pressed &&
                           (static_cast<QMouseEvent *>(e)->position().toPoint() -
                            m_pressPos).manhattanLength() < 6;
        m_pressed = false;
        if (click) {
            togglePlayPause();
            return true;
        }
        return false;
    }
    if (e->type() == QEvent::MouseButtonRelease)
        m_pressed = false;
    return QWidget::eventFilter(watched, e);
}

void VideoPane::setDefaults(bool muted, bool loop, int volume) {
    m_muteDefault = muted;
    m_audio->setMuted(muted);
    m_audio->setVolume(qBound(0, volume, 100) / 100.0);
    m_vol->setValue(qBound(0, volume, 100));
    m_player->setLoops(loop ? QMediaPlayer::Infinite : 1);
}

void VideoPane::setRate(double rate) {
    m_rate = qBound(0.25, rate, 2.0);
    m_player->setPlaybackRate(m_rate);
    if (m_rateBtn)
        m_rateBtn->setText(QString::number(m_rate) + QChar(0xD7)); // 1×
}

void VideoPane::setFile(const QString &path) {
    if (m_view)
        m_view->resetSize();
    setUrl(QUrl::fromLocalFile(path));
}

void VideoPane::setUrl(const QUrl &url, const Post &post) {
    // Drop temp file and cancel any in-flight web download before loading
    // the new source. Do NOT call m_player->stop() here: setSource(url)
    // stops playback and loads the new source directly; stopping first can
    // leave the FFmpeg backend in a state where the new source fails.
    if (m_webId >= 0 && m_webBackend) {
        m_webBackend->cancel(m_webId);
        m_webId = -1;
    }
    if (m_imgCache && !m_cacheUrl.isEmpty()) {
        m_imgCache->cancelUrl(m_cacheUrl);
        m_cacheUrl.clear();
    }
    m_fallbackTried = false;
    m_errorShown = false;
    ++m_errorGen; // cancel any deferred red from the previous video
    m_lastRemoteUrl.clear();
    dropTemp();
    // Pristine geometry for the incoming video: a stale scene rect
    // from the previous file would frame this one wrong (small or
    // cropped) until a size event lands.
    if (m_view)
        m_view->resetSize();
    m_audio->setMuted(m_muteDefault); // muted by default, every video
    m_buf->setVisible(false); // clear any stale error/buffering text
    // Fingerprint-walled hosts: QMediaPlayer's plain stack gets wall
    // pages, so buffer through the browser profile, then play local.
    // Site-aware (entry base host, not the file URL: media CDNs live
    // next door) so CDN-hosted videos route too.
    const bool remote = !url.isLocalFile();
    if (remote) {
        m_lastRemoteUrl = url.toString();
        m_lastRemotePost = post;
    }
    // A configured global proxy covers the QNAM stack only: the
    // player would stream past it (privacy hole, and often a plain
    // failure on filtered networks), so buffer through the cache.
    const bool viaCache = remote && m_imgCache &&
                          !RemotePosts::globalProxySpec().isEmpty();
    if (viaCache) {
        startCacheFetch(url, post);
        return;
    }
    const bool viaBrowser = remote && m_webBackend &&
                            RemotePosts::needsWebDownload(post);
    if (viaBrowser) {
        const QString ext = QFileInfo(url.path()).suffix().toLower();
        const QString dir =
            QStandardPaths::writableLocation(
                QStandardPaths::CacheLocation) +
            QStringLiteral("/box/video-tmp");
        QDir().mkpath(dir);
        m_tmpPath =
            dir + u'/' +
            QString::fromLatin1(
                QCryptographicHash::hash(url.toString().toUtf8(),
                                         QCryptographicHash::Sha1)
                    .toHex()) +
            (ext.isEmpty() ? QStringLiteral(".mp4") : u'.' + ext);
        // Unique per start, far above DownloadManager's 1-upwards ids:
        // the backend is shared and keys jobs by id, so the fixed 1 used
        // to collide with early downloads (their completions landed in the
        // player as ghost videos, and vice versa).
        static std::atomic<qint64> nextWebId{1LL << 62};
        m_webId = ++nextWebId;
        m_buf->setText(tr("buffering…"));
        m_buf->setVisible(true);
        m_webBackend->start(m_webId, url.toString(), m_tmpPath);
        return;
    }
    m_player->setSource(url);
    m_player->play();
}

void VideoPane::stop() {
    if (m_webId >= 0 && m_webBackend) {
        m_webBackend->cancel(m_webId);
        m_webId = -1;
    }
    if (m_imgCache && !m_cacheUrl.isEmpty()) {
        m_imgCache->cancelUrl(m_cacheUrl);
        m_cacheUrl.clear();
    }
    dropTemp();
    m_errorShown = false;
    ++m_errorGen; // cancel any deferred red
    m_player->stop();
    // Don't clear the source here: setSource(QUrl()) is async and can emit
    // InvalidMedia *after* setUrl() has already loaded the next video,
    // stranding the player in an error state. setUrl()'s setSource(url)
    // replaces whatever was playing directly.
    m_buf->setVisible(false); // clear any stale error/buffering text
    if (m_view)
        m_view->fit(); // next video starts fitted, not at the old zoom
}

void VideoPane::dropTemp() {
    if (!m_tmpPath.isEmpty()) {
        QFile::remove(m_tmpPath);
        m_tmpPath.clear();
    }
}

void VideoPane::setImageCache(ImageCache *cache) {
    if (m_imgCache == cache)
        return;
    if (m_imgCache)
        disconnect(m_imgCache, nullptr, this, nullptr);
    m_imgCache = cache;
    if (!m_imgCache)
        return;
    connect(m_imgCache, &ImageCache::ready, this, &VideoPane::onCacheReady);
    connect(m_imgCache, &ImageCache::failed, this, &VideoPane::onCacheFailed);
}

void VideoPane::startCacheFetch(const QUrl &url, const Post &post) {
    // Same hardened stack as stills: referer + cookies + redirects +
    // proxy, then play the local file. Reuses the disk cache when the
    // full image cache already fetched this URL.
    m_cacheUrl = url.toString();
    m_cachePost = post;
    m_buf->setText(tr("buffering\u2026"));
    m_buf->setStyleSheet(QStringLiteral("color:#7c9ef8;"));
    m_buf->setVisible(true);
    const QString have = m_imgCache->localPath(m_cacheUrl);
    if (!have.isEmpty()) {
        onCacheReady(m_cacheUrl, have);
        return;
    }
    m_imgCache->request(m_cacheUrl, RemotePosts::mediaRefererFor(post));
}

void VideoPane::onCacheReady(const QString &url, const QString &localPath) {
    if (url != m_cacheUrl)
        return; // stale (navigated away mid-fetch)
    m_cacheUrl.clear();
    m_errorShown = false;
    ++m_errorGen; // direct-stream error no longer applies
    m_buf->setVisible(false);
    m_player->setSource(QUrl::fromLocalFile(localPath));
    m_player->play();
}

void VideoPane::onCacheFailed(const QString &url, const QString &error) {
    if (url != m_cacheUrl)
        return;
    m_cacheUrl.clear();
    m_errorShown = true;
    m_buf->setText(error.isEmpty() ? tr("video unavailable") : error);
    m_buf->setStyleSheet(QStringLiteral("color:#e05555;"));
    m_buf->setVisible(true);
}

void VideoPane::onPlayerError(const QString &error) {
    // Frames are (or were) on screen: the stream renders, so this is
    // a blip (player backends cry wolf on seeks/stalls), not a
    // failure. Never yank playback or stick an error over video.
    if (m_view && m_view->hasFrames())
        return;
    // Direct streaming failed (referer walls, redirects, filtered
    // networks): retry once through the cached QNAM fetch instead of
    // leaving a dead pane.
    if (!m_fallbackTried && m_imgCache && !m_lastRemoteUrl.isEmpty()) {
        m_fallbackTried = true;
        startCacheFetch(QUrl(m_lastRemoteUrl), m_lastRemotePost);
        return;
    }
    // Defer the red label: slow starts emit terminal-looking errors
    // while still loading, which used to flash "can't play" over a
    // video that was about to render. Re-check at paint time and only
    // keep it for genuinely terminal states with no frames.
    const int gen = ++m_errorGen;
    const QString msg =
        error.isEmpty() ? tr("Cannot play this video") : tr("Video error: %1").arg(error);
    QTimer::singleShot(650, this, [this, gen, msg] {
        if (gen != m_errorGen)
            return; // navigated away / stopped / superseded
        if (m_view && m_view->hasFrames())
            return; // recovered while waiting
        if (m_player) {
            const QMediaPlayer::MediaStatus s = m_player->mediaStatus();
            if (s == QMediaPlayer::LoadingMedia ||
                s == QMediaPlayer::BufferingMedia ||
                s == QMediaPlayer::StalledMedia ||
                s == QMediaPlayer::BufferedMedia ||
                s == QMediaPlayer::LoadedMedia)
                return; // still (or now) loading: not terminal
        }
        m_errorShown = true;
        m_buf->setVisible(true);
        m_buf->setText(msg);
        m_buf->setStyleSheet(QStringLiteral("color:#e05555;"));
    });
}

void VideoPane::setWebBackend(WebDownloadBackend *backend) {
    if (m_webBackend == backend)
        return;
    m_webBackend = backend;
    if (!m_webBackend)
        return;
    connect(m_webBackend, &WebDownloadBackend::finished, this,
            &VideoPane::onWebFinished);
}

void VideoPane::onWebFinished(qint64 id, bool ok, const QString &error) {
    if (id != m_webId)
        return;
    m_webId = -1;
    if (ok && QFile::exists(m_tmpPath)) {
        m_buf->setVisible(false);
        m_player->setSource(QUrl::fromLocalFile(m_tmpPath));
        m_player->play();
    } else {
        dropTemp();
        m_buf->setText(
            error.isEmpty() ? tr("video unavailable") : error);
        m_buf->setVisible(true);
    }
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
    openBtn->setToolTip(tr("Open file"));
    auto *copyBtn = new QPushButton(tr("Copy file URL"), this);
    copyBtn->setIcon(UiIcons::icon(QStringLiteral("copy")));
    copyBtn->setIconSize(QSize(15, 15));
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
    connect(m_postBtn, &QPushButton::clicked, this, [this] {
        // Scheme-gated: a rogue sidecar/API postUrl like file:///etc
        // must never reach the local file handler on click.
        const QUrl url(m_posts[m_row].postUrl);
        if (!m_posts[m_row].postUrl.isEmpty() &&
            RemotePosts::isHttpUrl(m_posts[m_row].postUrl))
            QDesktopServices::openUrl(url);
    });
    connect(openBtn, &QPushButton::clicked, this, [this] {
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
    connect(copyBtn, &QPushButton::clicked, this, [this] {
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
    bar->addWidget(m_postBtn);
    bar->addWidget(m_imgSearchBtn);
    bar->addWidget(m_refreshBtn);
    bar->addWidget(notesBtn);
    bar->addWidget(m_favBtn);
    bar->addWidget(m_downloadBtn);
    bar->addWidget(openBtn);
    bar->addWidget(copyBtn);
    bar->addWidget(m_commentsBtn);
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

void ViewerPage::setPosts(QVector<Post> posts) {
    m_posts = std::move(posts);
}

void ViewerPage::setPlaybackDefaults(bool muted, bool loop, int volume) {
    m_video->setDefaults(muted, loop, volume);
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
    // Login-walled full files with public samples (ATF originals): one
    // fallback fetch before giving up, so the post still opens.
    if (!m_sampleFallback && m_row >= 0 && m_row < m_posts.size()) {
        const Post &p = m_posts.at(m_row);
        if (p.isRemote && url == p.fileUrl && !p.sampleUrl.isEmpty() &&
            p.sampleUrl != p.fileUrl && m_imgCache) {
            m_sampleFallback = true;
            m_pendingUrl = p.sampleUrl;
            showNote(tr("Full file unavailable — showing sample"));
            m_loadBar->setRange(0, 0); // busy until progress lands
            m_loadBar->setVisible(true);
            const QString have = m_imgCache->localPath(p.sampleUrl);
            if (!have.isEmpty())
                onCacheReady(p.sampleUrl, have);
            else
                m_imgCache->request(p.sampleUrl,
                                    RemotePosts::mediaRefererFor(p));
            return;
        }
    }
    m_pendingUrl.clear();
    m_loadBar->setVisible(false);
    m_loadInfo->setVisible(false);
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
    // Download speed + progress text next to the bar: "1.2MB / 12MB  ·  3.4MB/s"
    const qint64 now = m_loadTimer.isValid() ? m_loadTimer.elapsed() : 0;
    if (m_loadLastTime == 0) {
        m_loadTimer.start();
        m_loadLastTime = now;
        m_loadLastReceived = received;
    }
    const qint64 dt = now - m_loadLastTime;
    const qint64 dr = received - m_loadLastReceived;
    double speedBps = 0.0;
    if (dt > 100) { // sample no faster than 100ms
        speedBps = double(dr) / (double(dt) / 1000.0);
        m_loadLastTime = now;
        m_loadLastReceived = received;
    }
    QString info;
    if (total > 0) {
        info = formatBytes(received) + QStringLiteral(" / ") + formatBytes(total);
    } else {
        info = formatBytes(received);
    }
    if (speedBps > 0.0)
        info += QStringLiteral("  ·  ") + formatBytes(qint64(speedBps)) + QStringLiteral("/s");
    m_loadInfo->setText(info);
    m_loadInfo->setVisible(true);
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
        m_loadInfo->setVisible(false);
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

Post ViewerPage::currentPost() const {
    return hasCurrent() ? m_posts.at(m_row) : Post();
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
    m_sampleFallback = false;
    m_loadBar->setVisible(false);
    m_loadInfo->setVisible(false);
    showNote({});
    m_row = row;
    m_commentsExpanded = false; // long threads start collapsed, per post
    maybeFetchHybooruDetail();
    primeThumb(row);
    showCurrent();
    preloadNeighbours();
    setFocus();
}

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
    if (m_detailFetched.contains(key))
        return;
    m_detailFetched.insert(key);
    emit refreshRequested(p);
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
    // Reverse image search needs a fetchable remote file (engines cannot
    // take local paths).
    if (m_imgSearchBtn)
        m_imgSearchBtn->setEnabled(
            p.isRemote && RemotePosts::isHttpUrl(p.fileUrl));
    if (m_commentsBtn) {
        const auto itc = p.isRemote
                             ? m_metaCache.constFind(metaKey())
                             : m_metaCache.constEnd();
        const int n = itc != m_metaCache.constEnd() ? itc->second.size() : 0;
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
                // refresh may restore the URL — once per post per session,
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
                const QString have = m_imgCache->localPath(url);
                if (!have.isEmpty())
                    onCacheReady(url, have);
                else
                    m_imgCache->request(url, RemotePosts::mediaRefererFor(p));
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
        // Native custom categories (Szurubooru instances define their
        // own): one group each, accent colored.
        for (auto it = p.customTags.begin(); it != p.customTags.end();
             ++it) {
            if (it.value().isEmpty())
                continue;
            QString label = it.key();
            if (!label.isEmpty())
                label[0] = label[0].toUpper();
            addTagGroup(m_tagLayout, label, it.value().join(QChar::Space),
                        QStringLiteral("#7c9ef8"));
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
    // it was given, never the display label — lookups must match it or
    // cached comments are invisible and every view refetches.
    const QString key =
        !p.siteKey.isEmpty() ? p.siteKey : RemotePosts::keyForLabel(p.siteName);
    if (key.isEmpty())
        return {};
    return key + u'#' + QString::number(p.id);
}

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
            auto *l = new QLabel(
                tr("Notes/comments failed to load — press the reload "
                   "button to retry."),
                this);
            l->setWordWrap(true);
            l->setStyleSheet(QStringLiteral("color:#e0a055;font-size:11px;"));
            m_tagLayout->addWidget(l);
        }
        return; // still fetching (or failed, noted above)
    }
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
    if (site && !m_metaCache.contains(metaKey()))
        m_meta->fetch(key, site->baseUrl, p.id, site->engine);
}

void ViewerPage::onMetaDone(const QString &site, qint64 postId,
                             QVector<PostNote> notes, QVector<PostComment> comments, bool ok) {
    const QString key = site + u'#' + QString::number(postId);
    if (!ok) {
        // Failed fetch (network, HTTP error, challenge page): never cache
        // as definitive-empty, and keep any older marks untouched. The
        // thread UI shows the failure with a retry; reopening refetches
        // (the fetch gate only skips cached keys).
        m_metaFailed.insert(key);
        if (metaKey() == key) {
            if (m_commentsBtn)
                m_commentsBtn->setText(tr("Comments"));
            rebuildMetaTail();
        }
        return;
    }
    m_metaFailed.remove(key);
    m_metaCache.insert(key, {notes, comments});
    // Grid pills learn from the fetched tail too (OR-in downstream):
    // a post whose search row carried no stamp still earns its marks.
    emit metaMarksChanged(site, postId, !comments.isEmpty(), !notes.isEmpty());
    // Re-render only if the user is still on that post (tag host was rebuilt
    // per show, so re-run the tail of showCurrent cheaply: rebuild panel).
    if (metaKey() != site + u'#' + QString::number(postId) || m_tagLayout == nullptr)
        return;
    if (m_commentsBtn) {
        const auto it = m_metaCache.constFind(metaKey());
        const int n = it != m_metaCache.constEnd() ? it->second.size() : 0;
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
    if (p.isRemote)
        m_metaCache.remove(p.siteName + u'#' + QString::number(p.id));
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
// yet — avatarUrl is reserved), bold name · date, selectable body.
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
    dlg->setWindowTitle(p.isRemote ? tr("Comments — %1 #%2")
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
                auto *l = new QLabel(
                    tr("Couldn't load the thread (network or login "
                       "wall)."),
                    host);
                l->setWordWrap(true);
                l->setStyleSheet(QStringLiteral("color:#e0a055;"));
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
            } else if (it->second.isEmpty()) {
                auto *l = new QLabel(tr("No comments yet."), host);
                l->setWordWrap(true);
                l->setStyleSheet(QStringLiteral("color:#4a5068;"));
                vl->addWidget(l);
            } else {
                int shown = 0;
                for (const PostComment &c : it->second) {
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
                                   const QVector<PostComment> &, bool) {
                if (key.isEmpty() ||
                    key != site + u'#' + QString::number(postId))
                    return;
                if (m_commentsBtn && metaKey() == key) {
                    const auto it = m_metaCache.constFind(key);
                    const int n =
                        it != m_metaCache.constEnd() ? it->second.size() : 0;
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
    const auto it = m_metaCache.find(p.isRemote
                                         ? p.siteName + u'#' + QString::number(p.id)
                                         : QString());
    if (it == m_metaCache.end())
        m_image->setNotes({}, {});
    else
        m_image->setNotes(it->first, QSize(p.width, p.height));
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

void ViewerPage::keyPressEvent(QKeyEvent *e) {
    // Arrows are handled by global shortcuts (always switch posts);
    // keep Esc/Backspace here as a fallback for when shortcuts miss.
    if (e->key() == Qt::Key_Escape || e->key() == Qt::Key_Backspace)
        emit back();
    else
        QWidget::keyPressEvent(e);
}
