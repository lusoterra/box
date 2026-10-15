#pragma once

#include "post.h"
#include "postmeta.h"

#include <QHash>
#include <QLabel>
#include <QMovie>
#include <QProgressBar>
#include <QScrollArea>
#include <QVector>
#include <QWidget>

#include <QAudioOutput>
#include <QGraphicsPixmapItem>
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QMediaPlayer>
#include <QPushButton>
#include <QSlider>
#include <QStackedWidget>
#include <QVideoWidget>

class QBoxLayout;
class QAbstractItemModel;

class ImageCache;
class PostMetaFetcher;
class PostMetaFetcher;

// Zoomable/pannable still-image view: wheel zooms at cursor, drag pans,
// double-click toggles fit <-> 100%.
class ImageView : public QGraphicsView {
    Q_OBJECT
public:
    explicit ImageView(QWidget *parent = nullptr);
    void setImage(const QImage &img, bool applyNotes = true);
    void fit();
    void zoom100();
    // Annotation boxes (danbooru notes): rects in ORIGINAL image pixels;
    // refSize maps them onto whatever pixmap is actually displayed.
    // Hovering a box shows its body.
    void setNotes(const QVector<PostNote> &notes, const QSize &refSize);

protected:
    void wheelEvent(QWheelEvent *e) override;
    bool viewportEvent(QEvent *e) override; // wheel zooms only, never scrolls
    void mouseDoubleClickEvent(QMouseEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;

private:
    void applyZoom(double f, const QPoint &at);
    void applyPendingNotes();
    QGraphicsScene *m_scene;
    QGraphicsPixmapItem *m_item = nullptr;
    QVector<PostNote> m_pendingNotes;
    QSize m_refSize;
    double m_zoom = 1.0;
    bool m_fitted = true;
};

// Zoomable/pannable gif view (QMovie has no GraphicsItem, so QLabel +
// QMovie::setScaledSize + hand-dragged scroll area).
class GifView : public QWidget {
    Q_OBJECT
public:
    explicit GifView(QWidget *parent = nullptr);
    void setFile(const QString &path);
    void clear();

protected:
    void wheelEvent(QWheelEvent *e) override;
    bool eventFilter(QObject *watched, QEvent *e) override;

private:
    void applyZoom();
    QScrollArea *m_scroll;
    QLabel *m_label;
    QMovie *m_movie = nullptr;
    QSize m_base = QSize(320, 240);
    bool m_sized = false;
    double m_zoom = 1.0;
    QPoint m_dragFrom;
    bool m_dragging = false;
};

// mp4/webm playback via Qt Multimedia (FFmpeg backend). No libmpv needed.
class VideoPane : public QWidget {
    Q_OBJECT
public:
    explicit VideoPane(QWidget *parent = nullptr);
    void setFile(const QString &path);
    void setUrl(const QUrl &url);
    void stop();
    void setDefaults(bool muted, bool loop, int volume);

protected:
    bool eventFilter(QObject *watched, QEvent *e) override; // click video = pause/resume

private:
    void togglePlayPause();
    QVideoWidget *m_video;
    QMediaPlayer *m_player;
    QAudioOutput *m_audio;
    QPushButton *m_playBtn;
    QPushButton *m_muteBtn;
    QSlider *m_slider;
    QSlider *m_vol;
    QLabel *m_time;
    QLabel *m_buf;
    bool m_muteDefault = true;
};

// Seek bar that jumps to the click (plain QSlider only drags the knob or
// page-steps). Click-drag keeps scrubbing with live seeks.
class SeekSlider : public QSlider {
    Q_OBJECT
public:
    using QSlider::QSlider;

protected:
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;

private:
    void jumpTo(const QPoint &pos);
    bool m_scrubbing = false;
};

// Same-window post viewer: replaces the old popup dialog. Toolbar with
// back/prev/next + open-post/open-file actions, media area, metadata panel
// with clickable tags.
class ViewerPage : public QWidget {
    Q_OBJECT
public:
    explicit ViewerPage(QWidget *parent = nullptr);

    void setPosts(QVector<Post> posts);
    void showRow(int row);
    void setFaved(bool faved);
    void setRefreshable(bool ok, const QString &reason);
    // Instant prerender: show the (cached) grid thumbnail while the full
    // file downloads. Replaced by the real media when it arrives.
    void showThumbPreview(const QPixmap &thumb);
    void setPlaybackDefaults(bool muted, bool loop, int volume);
    void setMaxSide(int px) { m_maxSide = qMax(512, px); }
    void setImageCache(ImageCache *cache);
    // Grid model for instant stepping: the target row's thumbnail shows
    // the moment navigation lands, before its full file arrives.
    void setThumbModel(QAbstractItemModel *model);
    void setMetaCreds(const QString &danUser, const QString &danKey,
                      const QString &gelUser, const QString &gelKey);

signals:
    void back();
    void tagActivated(const QString &tag);
    void downloadRequested(const Post &post);
    void favouriteRequested(const Post &post);
    void refreshRequested(const Post &post);
    void pastEnd(); // nav stepped past the last loaded post

protected:
    void keyPressEvent(QKeyEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    bool eventFilter(QObject *watched, QEvent *e) override;

private slots:
    void onCacheReady(const QString &url, const QString &localPath);
    void onCacheFailed(const QString &url, const QString &error);
    void onCacheProgress(const QString &url, qint64 received, qint64 total);
    void onMetaDone(const QString &site, qint64 postId, QVector<PostNote> notes,
                    QVector<PostComment> comments);

private:
    void step(int d);
    void showCurrent();
    void stageNotes();
    void primeThumb(int row);      // instant grid thumbnail on navigation
    void preloadNeighbours();      // full files for row-1/row+1 into cache
    QString metaKey() const;
    void renderMetaSections();
    void showNote(const QString &text);
    void loadLocalImage(const QString &path, int row);
    void addTagGroup(QBoxLayout *lay, const QString &name, const QString &tags,
                     const QString &color);

    QVector<Post> m_posts;
    int m_row = 0;
    QStackedWidget *m_stack;
    ImageView *m_image;
    GifView *m_gif;
    VideoPane *m_video;
    QLabel *m_posLabel;
    QLabel *m_idLabel, *m_titleLabel, *m_dateLabel, *m_pathLabel;
    QLabel *m_scoreLabel, *m_ratingLabel, *m_sizeLabel, *m_extLabel, *m_srcLabel;
    QPushButton *m_prevBtn, *m_nextBtn, *m_postBtn, *m_fitBtn, *m_zoomBtn;
    QPushButton *m_downloadBtn = nullptr;
    QPushButton *m_favBtn = nullptr;
    QPushButton *m_refreshBtn = nullptr;
    QLabel *m_note = nullptr;
    QProgressBar *m_loadBar;
    QScrollArea *m_tagScroll = nullptr;
    QWidget *m_tagHost = nullptr;
    QBoxLayout *m_tagLayout = nullptr;
    int m_maxSide = 2560;
    ImageCache *m_imgCache = nullptr;
    PostMetaFetcher *m_meta = nullptr;
    // (site#id) -> notes+comments, so back/forth never refetches.
    QHash<QString, QPair<QVector<PostNote>, QVector<PostComment>>> m_metaCache;
    QString m_pendingUrl; // remote file currently being fetched
    int m_pendingRow = -1;
    QPixmap m_pendingThumb; // grid thumbnail shown until the full file lands
    QAbstractItemModel *m_thumbModel = nullptr; // grid model (thumb source)
};
