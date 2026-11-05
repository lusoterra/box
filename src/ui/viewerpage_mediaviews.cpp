#include "viewerpage.h"

#include "imagecache.h"
#include "uicons.h"
#include "webdlbackend.h"

#include <QApplication>
#include <QAudioDevice>
#include <QAudioOutput>
#include <QCryptographicHash>
#include <QDir>
#include <QDrag>
#include <QFile>
#include <QFileInfo>
#include <QGraphicsEllipseItem>
#include <QGraphicsRectItem>
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QHBoxLayout>
#include <QLabel>
#include <QMediaDevices>
#include <QMediaPlayer>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QMovie>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QSlider>
#include <QStandardPaths>
#include <QStyle>
#include <QStyleOptionSlider>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QVideoSink>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace {
void beginUrlDrag(QWidget *source, const QPixmap &preview,
                  const QString &url) {
    if (url.isEmpty())
        return;
    auto *mime = new QMimeData();
    mime->setUrls({QUrl(url)});
    mime->setText(url);
    auto *drag = new QDrag(source);
    drag->setMimeData(mime);
    if (!preview.isNull())
        drag->setPixmap(preview.scaled(128, 128, Qt::KeepAspectRatio,
                                       Qt::SmoothTransformation));
    drag->exec(Qt::CopyAction);
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
        // No Qt title-tooltip: a left click opens the readable popup.
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
    if (!m_item || e->button() != Qt::LeftButton)
        return;
    emit fullscreenToggleRequested();
    e->accept();
}

void ImageView::contextMenuEvent(QContextMenuEvent *e) {
    emit contextMenuRequested(e->globalPos());
    e->accept();
}

void ImageView::mousePressEvent(QMouseEvent *e) {
    if (e->button() == Qt::LeftButton) {
        const QPointF scenePos = mapToScene(e->pos());
        const int idx = m_noteBoxes.indexOf(
            static_cast<QGraphicsRectItem *>(scene()->itemAt(
                scenePos, QTransform())));
        if (idx >= 0 && idx < m_notes.size()) {
            emit noteActivated(m_notes.at(idx).body, e->globalPosition().toPoint());
            e->accept();
            return;
        }
    }
    // At fit there is nothing to pan: a left drag exports the post URL.
    if (e->button() == Qt::LeftButton && m_fitted && !m_dragUrl.isEmpty()) {
        m_dragPress = e->pos();
        m_dragMaybe = true;
    }
    QGraphicsView::mousePressEvent(e);
}

void ImageView::mouseMoveEvent(QMouseEvent *e) {
    if (m_dragMaybe && (e->buttons() & Qt::LeftButton) &&
        (e->pos() - m_dragPress).manhattanLength() >=
            QApplication::startDragDistance()) {
        m_dragMaybe = false;
        beginUrlDrag(this, grab(), m_dragUrl);
        return;
    }
    QGraphicsView::mouseMoveEvent(e);
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
    // before - this was the blank-gif bug); every frame after that just
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
    if (!m_item || e->button() != Qt::LeftButton)
        return;
    emit fullscreenToggleRequested();
    e->accept();
}

void GifView::contextMenuEvent(QContextMenuEvent *e) {
    emit contextMenuRequested(e->globalPos());
    e->accept();
}

void GifView::mousePressEvent(QMouseEvent *e) {
    if (e->button() == Qt::LeftButton && m_fitted && !m_dragUrl.isEmpty()) {
        m_dragPress = e->pos();
        m_dragMaybe = true;
    }
    QGraphicsView::mousePressEvent(e);
}

void GifView::mouseMoveEvent(QMouseEvent *e) {
    if (m_dragMaybe && (e->buttons() & Qt::LeftButton) &&
        (e->pos() - m_dragPress).manhattanLength() >=
            QApplication::startDragDistance()) {
        m_dragMaybe = false;
        beginUrlDrag(this, grab(), m_dragUrl);
        return;
    }
    QGraphicsView::mouseMoveEvent(e);
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
        // and has preceded a SIGSEGV in the backend - never attempt it.
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
    if (e->button() != Qt::LeftButton)
        return;
    emit fullscreenToggleRequested();
    e->accept();
}

void VideoView::contextMenuEvent(QContextMenuEvent *e) {
    emit contextMenuRequested(e->globalPos());
    e->accept();
}

void VideoView::mousePressEvent(QMouseEvent *e) {
    if (e->button() == Qt::LeftButton && m_fitted && !m_dragUrl.isEmpty()) {
        m_dragPress = e->pos();
        m_dragMaybe = true;
    }
    QGraphicsView::mousePressEvent(e);
}

void VideoView::mouseMoveEvent(QMouseEvent *e) {
    if (m_dragMaybe && (e->buttons() & Qt::LeftButton) &&
        (e->pos() - m_dragPress).manhattanLength() >=
            QApplication::startDragDistance()) {
        m_dragMaybe = false;
        beginUrlDrag(this, grab(), m_dragUrl);
        return;
    }
    QGraphicsView::mouseMoveEvent(e);
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
    connect(m_view, &VideoView::fullscreenToggleRequested, this,
            &VideoPane::fullscreenToggleRequested);
    connect(m_view, &VideoView::contextMenuRequested, this,
            &VideoPane::contextMenuRequested);
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
    m_buf->setCursor(Qt::PointingHandCursor);
    m_buf->setToolTip(tr("Click for request details"));
    m_buf->installEventFilter(this);
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
            // Only the source we asked for, and only while playback is
            // still wanted: a stale tick from the previous video used to
            // restart it (audio from 0:00 over a still/another post).
            if (m_wanted &&
                (m_expectedSource.isEmpty() ||
                 m_player->source() == m_expectedSource)) {
                m_player->setPlaybackRate(
                    m_rate); // stick across videos in-session
                m_player->play(); // setSource() is async: play when ready
            }
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
    if (watched == m_buf && e->type() == QEvent::MouseButtonPress &&
        m_errorShown &&
        static_cast<QMouseEvent *>(e)->button() == Qt::LeftButton) {
        emit errorDetailsRequested(
            m_lastRemoteUrl,
            m_buf->text().isEmpty() ? tr("Media failed") : m_buf->text());
        return true;
    }
    // Left-click on the picture toggles pause; anything else (side
    // buttons included) keeps its normal path to the parent viewer.
    // NOTE: drag-pan starts with a left press too - the click only
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
        // setSource() lands only when the fetch completes: halt the old
        // video now or it plays on through the buffering phase.
        m_player->stop();
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
        // setSource() lands only when the browser fetch completes: halt
        // the old video now or it plays on (the "still playing the first
        // video" bug) through the whole buffering phase.
        m_player->stop();
        m_webBackend->start(m_webId, url.toString(), m_tmpPath);
        return;
    }
    m_wanted = true;
    m_expectedSource = url;
    m_player->setSource(url);
    m_player->play();
}

void VideoPane::stop() {
    m_wanted = false;
    m_expectedSource.clear();
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
    m_audio->setMuted(true); // no ghost audio if a backend plays on
    // Don't clear the source here: setSource(QUrl()) is async and can emit
    // InvalidMedia *after* setUrl() has already loaded the next video,
    // stranding the player in an error state. setUrl()'s setSource(url)
    // replaces whatever was playing directly; m_wanted gates stale play().
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
    const QString have = m_imgCache->localPath(m_cacheUrl, &post);
    if (!have.isEmpty()) {
        onCacheReady(m_cacheUrl, have);
        return;
    }
    m_imgCache->request(m_cacheUrl, RemotePosts::mediaRefererFor(post), &post);
}

void VideoPane::onCacheReady(const QString &url, const QString &localPath) {
    if (url != m_cacheUrl)
        return; // stale (navigated away mid-fetch)
    m_cacheUrl.clear();
    m_errorShown = false;
    ++m_errorGen; // direct-stream error no longer applies
    m_buf->setVisible(false);
    // Unload the previous source first: stop() keeps it loaded, and a
    // play() against a half-loaded new source resumes the OLD video
    // (the "still playing the first video" bug). The empty source's
    // async InvalidMedia is swallowed by the error-gen bump above.
    m_wanted = true;
    m_expectedSource = QUrl::fromLocalFile(localPath);
    m_player->setSource(QUrl());
    m_player->setSource(m_expectedSource);
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
    emit mediaFailed(url, error.isEmpty() ? tr("video unavailable") : error);
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
        // Unload the previous source first (see onCacheReady): stop()
        // keeps it loaded, so play() against a half-loaded source would
        // resume the OLD video.
        ++m_errorGen;
        m_wanted = true;
        m_expectedSource = QUrl::fromLocalFile(m_tmpPath);
        m_player->setSource(QUrl());
        m_player->setSource(m_expectedSource);
        m_player->play();
    } else {
        dropTemp();
        m_buf->setText(
            error.isEmpty() ? tr("video unavailable") : error);
        m_buf->setVisible(true);
        emit mediaFailed(m_lastRemoteUrl,
                         error.isEmpty() ? tr("video unavailable") : error);
    }
}

// ── ViewerPage ───────────────────────────────────────────────────

