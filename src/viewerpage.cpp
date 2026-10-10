#include "viewerpage.h"

#include <QApplication>
#include <QBoxLayout>
#include <QClipboard>
#include <QDesktopServices>
#include <QFileInfo>
#include <QFutureWatcher>
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
#include <QUrl>
#include <QVBoxLayout>
#include <QWheelEvent>
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

void ImageView::setImage(const QImage &img) {
    m_scene->clear();
    m_item = nullptr;
    if (img.isNull())
        return;
    m_item = m_scene->addPixmap(QPixmap::fromImage(img));
    m_scene->setSceneRect(m_item->boundingRect());
    fit();
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
    applyZoom(e->angleDelta().y() > 0 ? 1.15 : 1.0 / 1.15, e->position().toPoint());
    e->accept();
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
}

void GifView::setFile(const QString &path) {
    clear();
    m_movie = new QMovie(path, QByteArray(), this);
    if (!m_movie->isValid()) {
        delete m_movie;
        m_movie = nullptr;
        m_label->setText(QStringLiteral("Cannot play gif"));
        return;
    }
    // currentPixmap() before start() is 0x0 — and QSize(0,0).isValid() is
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
    m_zoom = qBound(0.1, m_zoom * (e->angleDelta().y() > 0 ? 1.15 : 1.0 / 1.15), 4.0);
    applyZoom();
    e->accept();
}

bool GifView::eventFilter(QObject *watched, QEvent *e) {
    // Hand-drag panning for the gif label.
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

VideoPane::VideoPane(QWidget *parent) : QWidget(parent) {
    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    m_video = new QVideoWidget(this);
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
    m_playBtn = new QPushButton(QStringLiteral("⏸"), this);
    m_playBtn->setFixedWidth(36);
    m_slider = new QSlider(Qt::Horizontal, this);
    m_time = new QLabel(QStringLiteral("0:00 / 0:00"), this);
    m_time->setStyleSheet(QStringLiteral("color:#4a5068;"));
    m_muteBtn = new QPushButton(QStringLiteral("🔇"), this);
    m_muteBtn->setFixedWidth(36);
    m_muteBtn->setToolTip(QStringLiteral("Mute / unmute"));
    m_vol = new QSlider(Qt::Horizontal, this);
    m_vol->setRange(0, 100);
    m_vol->setValue(100);
    m_vol->setFixedWidth(80);
    m_vol->setToolTip(QStringLiteral("Volume (system audio device)"));
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
            m_buf->setText(QStringLiteral("loading…"));
    });

    connect(m_muteBtn, &QPushButton::clicked, this, [this] {
        m_audio->setMuted(!m_audio->isMuted());
    });
    connect(m_audio, &QAudioOutput::mutedChanged, this, [this](bool m) {
        m_muteBtn->setText(m ? QStringLiteral("🔇") : QStringLiteral("🔊"));
    });
    connect(m_vol, &QSlider::valueChanged, this, [this](int v) {
        m_audio->setVolume(v / 100.0);
        if (v > 0 && m_audio->isMuted())
            m_audio->setMuted(false);
    });

    connect(m_playBtn, &QPushButton::clicked, this, [this] {
        if (m_player->playbackState() == QMediaPlayer::PlayingState)
            m_player->pause();
        else
            m_player->play();
    });
    connect(m_player, &QMediaPlayer::playbackStateChanged, this, [this] {
        m_playBtn->setText(m_player->playbackState() == QMediaPlayer::PlayingState
                               ? QStringLiteral("⏸")
                               : QStringLiteral("▶"));
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

void VideoPane::setDefaults(bool muted, bool loop, int volume) {
    m_muteDefault = muted;
    m_audio->setMuted(muted);
    m_audio->setVolume(qBound(0, volume, 100) / 100.0);
    m_vol->setValue(qBound(0, volume, 100));
    m_player->setLoops(loop ? QMediaPlayer::Infinite : 1);
}

void VideoPane::setFile(const QString &path) {
    m_audio->setMuted(m_muteDefault); // muted by default, every video
    m_player->setSource(QUrl::fromLocalFile(path));
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

    auto *bar = new QHBoxLayout();
    auto *backBtn = new QPushButton(QStringLiteral("‹ Back"), this);
    m_prevBtn = new QPushButton(QStringLiteral("‹"), this);
    m_nextBtn = new QPushButton(QStringLiteral("›"), this);
    m_posLabel = new QLabel(this);
    m_posLabel->setStyleSheet(QStringLiteral("color:#4a5068;"));
    m_fitBtn = new QPushButton(QStringLiteral("Fit"), this);
    m_zoomBtn = new QPushButton(QStringLiteral("100%"), this);
    m_postBtn = new QPushButton(QStringLiteral("Open post ↗"), this);
    auto *openBtn = new QPushButton(QStringLiteral("Open file"), this);
    auto *copyBtn = new QPushButton(QStringLiteral("Copy path"), this);
    connect(backBtn, &QPushButton::clicked, this, &ViewerPage::back);
    connect(m_prevBtn, &QPushButton::clicked, this, [this] { showRow(m_row - 1); });
    connect(m_nextBtn, &QPushButton::clicked, this, [this] { showRow(m_row + 1); });
    connect(m_fitBtn, &QPushButton::clicked, m_image, &ImageView::fit);
    connect(m_zoomBtn, &QPushButton::clicked, m_image, &ImageView::zoom100);
    connect(m_postBtn, &QPushButton::clicked, this, [this] {
        if (!m_posts[m_row].postUrl.isEmpty())
            QDesktopServices::openUrl(QUrl(m_posts[m_row].postUrl));
    });
    connect(openBtn, &QPushButton::clicked, this, [this] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(m_posts[m_row].absFile));
    });
    connect(copyBtn, &QPushButton::clicked, this, [this] {
        QApplication::clipboard()->setText(m_posts[m_row].absFile);
    });
    bar->addWidget(backBtn);
    bar->addWidget(m_prevBtn);
    bar->addWidget(m_nextBtn);
    bar->addWidget(m_posLabel);
    bar->addWidget(m_fitBtn);
    bar->addWidget(m_zoomBtn);
    m_loadBar = new QProgressBar(this);
    m_loadBar->setRange(0, 0); // busy indicator
    m_loadBar->setFixedWidth(120);
    m_loadBar->setTextVisible(false);
    m_loadBar->setVisible(false);
    bar->addWidget(m_loadBar);
    bar->addStretch(1);
    bar->addWidget(m_postBtn);
    bar->addWidget(openBtn);
    bar->addWidget(copyBtn);
    lay->addLayout(bar);

    auto *body = new QHBoxLayout();
    body->addWidget(m_stack, 1);

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
    const QStringList heads = {QStringLiteral("Score"), QStringLiteral("Rating"),
                               QStringLiteral("Size"), QStringLiteral("Type")};
    QLabel *vals[] = {m_scoreLabel, m_ratingLabel, m_sizeLabel, m_extLabel};
    for (int i = 0; i < 4; i++) {
        auto *h = new QLabel(heads[i], this);
        h->setStyleSheet(QStringLiteral("color:#4a5068;font-size:9px;"));
        grid->addWidget(h, i / 2 * 2, i % 2);
        grid->addWidget(vals[i], i / 2 * 2 + 1, i % 2);
    }
    pl->addLayout(grid);
    auto *th = new QLabel(QStringLiteral("TAGS"), this);
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
    // (seek slider included — it stays mouse-draggable).
    auto *left = new QShortcut(QKeySequence(Qt::Key_Left), this);
    left->setContext(Qt::WidgetWithChildrenShortcut);
    connect(left, &QShortcut::activated, this, [this] { showRow(m_row - 1); });
    auto *right = new QShortcut(QKeySequence(Qt::Key_Right), this);
    right->setContext(Qt::WidgetWithChildrenShortcut);
    connect(right, &QShortcut::activated, this, [this] { showRow(m_row + 1); });
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
            showRow(m_row - 1);
            return true;
        }
        if (button == Qt::ForwardButton) {
            showRow(m_row + 1);
            return true;
        }
    }
    return QWidget::eventFilter(watched, e);
}

void ViewerPage::mousePressEvent(QMouseEvent *e) {
    if (e->button() == Qt::BackButton) {
        showRow(m_row - 1);
        e->accept();
    } else if (e->button() == Qt::ForwardButton) {
        showRow(m_row + 1);
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

void ViewerPage::showRow(int row) {
    if (row < 0 || row >= m_posts.size() || m_posts.isEmpty())
        return;
    // stop previous media
    m_video->stop();
    m_gif->clear();
    m_row = row;
    showCurrent();
    setFocus();
}

void ViewerPage::showCurrent() {
    const Post &p = m_posts[m_row];
    m_posLabel->setText(QStringLiteral("%1 / %2").arg(m_row + 1).arg(m_posts.size()));
    m_prevBtn->setEnabled(m_row > 0);
    m_nextBtn->setEnabled(m_row + 1 < m_posts.size());
    m_postBtn->setEnabled(!p.postUrl.isEmpty());
    m_postBtn->setToolTip(p.postUrl);

    if (isVideoExt(p.ext)) {
        m_loadBar->setVisible(false);
        m_stack->setCurrentWidget(m_video);
        m_video->setFile(p.absFile);
    } else if (p.ext == QStringLiteral("gif")) {
        m_loadBar->setVisible(false);
        m_stack->setCurrentWidget(m_gif);
        m_gif->setFile(p.absFile);
    } else {
        m_stack->setCurrentWidget(m_image);
        // Decode off the UI thread. Huge originals are downscaled at decode
        // time (viewerMaxSide setting) — full-res pixmaps stall the view.
        m_loadBar->setVisible(true);
        QFutureWatcher<QImage> *w = new QFutureWatcher<QImage>(this);
        connect(w, &QFutureWatcher<QImage>::finished, this, [this, w, row = m_row] {
            m_loadBar->setVisible(false);
            if (row == m_row)
                m_image->setImage(w->result());
            w->deleteLater();
        });
        w->setFuture(QtConcurrent::run([path = p.absFile, cap = m_maxSide] {
            QImageReader reader(path);
            reader.setAutoTransform(true);
            const QSize full = reader.size();
            if (full.isValid() &&
                (full.width() > cap || full.height() > cap)) {
                const double k =
                    qMin(double(cap) / full.width(), double(cap) / full.height());
                reader.setScaledSize(
                    QSize(qMax(1, int(full.width() * k)), qMax(1, int(full.height() * k))));
            }
            return reader.read();
        }));
    }

    m_idLabel->setText(p.id ? QStringLiteral("#%1").arg(p.id) : QString());
    m_titleLabel->setText(p.title);
    m_dateLabel->setText(p.date);
    m_pathLabel->setText(p.file);
    m_scoreLabel->setText(p.hasMeta ? QStringLiteral("%1%2")
                                          .arg(p.score > 0 ? QStringLiteral("+")
                                                           : QStringLiteral(""))
                                          .arg(p.score)
                                    : QStringLiteral("—"));
    m_ratingLabel->setText(p.rating +
                           (p.ratingInferred ? QStringLiteral(" (inferred)") : QString()));
    m_sizeLabel->setText((p.width && p.height)
                             ? QStringLiteral("%1×%2").arg(p.width).arg(p.height)
                             : QStringLiteral("—"));
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
        auto *l = new QLabel(QStringLiteral("No metadata"), this);
        l->setStyleSheet(QStringLiteral("color:#4a5068;"));
        m_tagLayout->addWidget(l);
    } else {
        addTagGroup(m_tagLayout, QStringLiteral("Artist"), p.tagsArtist,
                    QStringLiteral("#d4955a"));
        addTagGroup(m_tagLayout, QStringLiteral("Copyright"), p.tagsCopyright,
                    QStringLiteral("#b570e0"));
        addTagGroup(m_tagLayout, QStringLiteral("Character"), p.tagsCharacter,
                    QStringLiteral("#60b880"));
        addTagGroup(m_tagLayout, QStringLiteral("General"), p.tagsGeneral,
                    QStringLiteral("#7aaee0"));
        addTagGroup(m_tagLayout, QStringLiteral("Meta"), p.tagsMeta,
                    QStringLiteral("#88aa88"));
    }
    m_tagLayout->addStretch(1);
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
