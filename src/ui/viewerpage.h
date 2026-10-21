#pragma once

#include "post.h"
#include "postmeta.h"

#include <QHash>
#include <QLabel>
#include <QMovie>
#include <QProgressBar>
#include <QScrollArea>
#include <QSet>
#include <QVector>
#include <QWidget>
#include <QDialog>
#include <QDialogButtonBox>
#include <QElapsedTimer>

#include <QAudioOutput>
#include <QGraphicsPixmapItem>
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QMediaPlayer>
#include <QPushButton>
#include <QSlider>
#include <QStackedWidget>
#include <QVideoFrame>

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
    // Hovering a box shows its body. Toggleable via setNotesVisible
    // (YouTube-2012-style overlays, on by default).
    void setNotes(const QVector<PostNote> &notes, const QSize &refSize);
    void setNotesVisible(bool visible);

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
    QVector<PostNote> m_notes; // staged, scaled onto m_item on apply
    QSize m_refSize;
    QList<QGraphicsRectItem *> m_noteBoxes; // live overlays (zoom with image)
    bool m_notesVisible = true; // overlay toggle (toolbar Notes button)
    double m_zoom = 1.0;
    bool m_fitted = true;
};

// Zoomable/pannable gif view: same QGraphicsView machinery as stills
// (frames land on a pixmap item, the view transform zooms), so wheel,
// drag and anchors behave identically — no label/scrollbar fighting.
class GifView : public QGraphicsView {
    Q_OBJECT
public:
    explicit GifView(QWidget *parent = nullptr);
    void setFile(const QString &path);
    void clear();
    void fit();
    void zoom100();

protected:
    void wheelEvent(QWheelEvent *e) override;
    bool viewportEvent(QEvent *e) override; // wheel zooms only, never scrolls
    void mouseDoubleClickEvent(QMouseEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;

private:
    void applyZoom(double f);
    QGraphicsScene *m_scene;
    QGraphicsPixmapItem *m_item = nullptr;
    QMovie *m_movie = nullptr;
    double m_zoom = 1.0;
    bool m_fitted = true;
};

// Frame-driven video item: sizes itself from the decoded frames it
// actually receives. Player metadata / nativeSizeChanged stay silent
// for some containers, but frames always carry dimensions — so a
// rendering video can never be stuck sizeless (tiny top-left).
class VideoFrameItem : public QGraphicsItem {
public:
    explicit VideoFrameItem(QGraphicsItem *parent = nullptr);
    // An invalid frame clears the image (used by resetSize).
    void setFrame(const QVideoFrame &frame);
    QRectF boundingRect() const override;
    void paint(QPainter *painter, const QStyleOptionGraphicsItem *option,
               QWidget *widget) override;

private:
    QImage m_image;
    QSizeF m_size; // display size (pixel-aspect honored)
};

class QVideoSink;

// mp4/webm playback via Qt Multimedia (FFmpeg backend). No libmpv needed.
// Zoomable/pannable like stills and gifs: frames land on a graphics item
// fed by a video sink, so wheel-zoom-at-cursor, drag-pan and double-click
// fit<->100% behave identically across all three.
class VideoView : public QGraphicsView {
    Q_OBJECT
public:
    explicit VideoView(QWidget *parent = nullptr);
    QVideoSink *videoSink() const { return m_sink; }
    // True once frames have sized the scene (i.e. the stream renders).
    bool hasFrames() const;
    // Pristine state for a new video: drops the frame, any stale scene
    // rect + zoom so the next frames fit cleanly instead of inheriting
    // the previous video's geometry (small/corner/big flashes).
    void resetSize();
    void fit();
    void zoom100();

protected:
    void wheelEvent(QWheelEvent *e) override;
    bool viewportEvent(QEvent *e) override; // wheel zooms only, never scrolls
    void mouseDoubleClickEvent(QMouseEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;

private:
    void applyZoom(double f);
    QGraphicsScene *m_scene;
    VideoFrameItem *m_item = nullptr;
    QVideoSink *m_sink = nullptr;
    double m_zoom = 1.0;
    bool m_fitted = true;
};

class VideoPane : public QWidget {
    Q_OBJECT
public:
    explicit VideoPane(QWidget *parent = nullptr);
    void setFile(const QString &path);
    void setUrl(const QUrl &url, const Post &post = {});
    void stop();
    void setDefaults(bool muted, bool loop, int volume);
    // Playback rate for the session (sticks across videos until changed).
    void setRate(double rate);
    // Zoom controls for the toolbar (route to the live video view).
    void fitView();
    void zoom100View();
    // Browser backend for fingerprint-walled video (null = stream direct).
    void setWebBackend(class WebDownloadBackend *backend);
    // QNAM-backed fetch for remote videos: used up front when a
    // global proxy is set (the player stack would bypass it) and as
    // a fallback when direct streaming errors out (referer/wall/
    // redirect cases the player cannot ride through itself).
    void setImageCache(class ImageCache *cache);

protected:
    bool eventFilter(QObject *watched, QEvent *e) override; // click video = pause/resume

private:
    void togglePlayPause();
    void onWebFinished(qint64 id, bool ok, const QString &error);
    void startCacheFetch(const QUrl &url, const Post &post);
    void onCacheReady(const QString &url, const QString &localPath);
    void onCacheFailed(const QString &url, const QString &error);
    void onPlayerError(const QString &error);
    void dropTemp();
    VideoView *m_view;
    QMediaPlayer *m_player;
    QAudioOutput *m_audio;
    QPushButton *m_playBtn;
    QPushButton *m_muteBtn;
    // Click-vs-pan disambiguation for pause-on-click (see eventFilter).
    QPoint m_pressPos;
    bool m_pressed = false;    QSlider *m_slider;
    class ImageCache *m_imgCache = nullptr;
    QString m_cacheUrl;   // remote fetch currently being buffered
    Post m_cachePost;     // post behind that fetch (referer)
    bool m_fallbackTried = false; // one cache retry per video max
    bool m_errorShown = false; // sticky red error currently up
    int m_errorGen = 0;         // cancels deferred error text on nav/stop/frames
    QString m_lastRemoteUrl;      // direct-stream URL (for retry)
    Post m_lastRemotePost;
    QSlider *m_vol;
    QPushButton *m_rateBtn = nullptr;
    double m_rate = 1.0; // session playback rate, applied on every load
    QLabel *m_time;
    QLabel *m_buf;
    bool m_muteDefault = true;
    class WebDownloadBackend *m_webBackend = nullptr;
    qint64 m_webId = -1; // in-flight browser download, if any
    QString m_tmpPath;   // buffered video file, removed on stop()
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
    // The post on screen (default Post when the viewer is empty).
    Post currentPost() const;
    bool hasCurrent() const;
    int currentRow() const;
    void setFaved(bool faved);
    void setDownloaded(bool downloaded); // green toggle when on disk
    // Status marks next to the title: AI-generated / favourited /
    // downloaded. Refreshed live while the viewer stays open.
    void setStatusBadges(bool ai, bool faved, bool downloaded);
    void setRefreshable(bool ok, const QString &reason);
    // Instant prerender: show the (cached) grid thumbnail while the full
    // file downloads. Replaced by the real media when it arrives.
    void showThumbPreview(const QPixmap &thumb);
    void setPlaybackDefaults(bool muted, bool loop, int volume);
    // Quit path: halt video playback (and its buffered fetch) now so the
    // player teardown never holds the close event open.
    void stopPlayback();
    // HyBooru summaries carry no tags: fetch the detail object the first
    // time such a post opens (normal refresh path paints tags on arrival).
    void maybeFetchHybooruDetail();
    void setMaxSide(int px) { m_maxSide = qMax(512, px); }
    void setImageCache(ImageCache *cache);
    void setWebBackend(class WebDownloadBackend *backend);
    // Grid model for instant stepping: the target row's thumbnail shows
    // the moment navigation lands, before its full file arrives.
    void setThumbModel(QAbstractItemModel *model);
    void setMetaCreds(const QString &danUser, const QString &danKey,
                      const QString &gelUser, const QString &gelKey);

signals:
    void back();
    void tagActivated(const QString &tag);
    // Right-click on a tag button (globalPos for the menu).
    void tagContextRequested(const QString &tag, const QPoint &globalPos);
    void downloadRequested(const Post &post);
    void deleteRequested(const Post &post); // button acts as delete toggle
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
    void showCommentsPopup(); // thread dialog for the current post
    void onMetaDone(const QString &site, qint64 postId, QVector<PostNote> notes,
                    QVector<PostComment> comments);

private:
    void step(int d);
    void showCurrent();
    void stageNotes();
    // Drop the notes/comments tail and re-render it (comment toggle,
    // fetch arrival); tag groups above stay untouched.
    void rebuildMetaTail();
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
    QLabel *m_aiBadge = nullptr;
    QLabel *m_favBadge = nullptr;
    QLabel *m_dlBadge = nullptr;
    QStackedWidget *m_stack;
    ImageView *m_image;
    GifView *m_gif;
    VideoPane *m_video;
    QLabel *m_posLabel;
    QLabel *m_idLabel, *m_titleLabel, *m_dateLabel, *m_pathLabel;
    QLabel *m_scoreLabel, *m_ratingLabel, *m_sizeLabel, *m_extLabel, *m_srcLabel;
    QPushButton *m_prevBtn, *m_nextBtn, *m_postBtn, *m_fitBtn, *m_zoomBtn;
    QPushButton *m_imgSearchBtn = nullptr; // reverse image search menu
    QPushButton *m_downloadBtn = nullptr;
    QPushButton *m_favBtn = nullptr;
    QPushButton *m_refreshBtn = nullptr;
    bool m_downloaded = false; // current post is on disk
    QPushButton *m_commentsBtn = nullptr; // popup with the post's thread
    QLabel *m_note = nullptr;
    QProgressBar *m_loadBar;
    QLabel *m_loadInfo; // "X.YMB / 12MB  ·  3.4MB/s" next to the bar
    QElapsedTimer m_loadTimer; // download speed sampling
    qint64 m_loadLastReceived = 0; // bytes at last speed sample
    qint64 m_loadLastTime = 0; // ms at last speed sample
    QScrollArea *m_tagScroll = nullptr;
    QWidget *m_tagHost = nullptr;
    QBoxLayout *m_tagLayout = nullptr;
    int m_maxSide = 2560;
    ImageCache *m_imgCache = nullptr;
    PostMetaFetcher *m_meta = nullptr;
    // (site#id) -> notes+comments, so back/forth never refetches.
    QHash<QString, QPair<QVector<PostNote>, QVector<PostComment>>> m_metaCache;
    // Comments collapse state: long threads show the first few behind a
    // show-all toggle (reset on every post change).
    bool m_commentsExpanded = false;
    // Layout index where the notes/comments sections start (toggle
    // rebuilds from here without touching the tag groups above).
    int m_metaSectionStart = 0;
    // HyBooru details already fetched this session (site#id): genuinely
    // tagless posts must not refetch on every open.
    QSet<QString> m_detailFetched;
    QString m_pendingUrl; // remote file currently being fetched
    int m_pendingRow = -1;
    QPixmap m_pendingThumb; // grid thumbnail shown until the full file lands
    QAbstractItemModel *m_thumbModel = nullptr; // grid model (thumb source)
};
