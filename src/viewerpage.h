#pragma once

#include "post.h"

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

// Zoomable/pannable still-image view: wheel zooms at cursor, drag pans,
// double-click toggles fit <-> 100%.
class ImageView : public QGraphicsView {
    Q_OBJECT
public:
    explicit ImageView(QWidget *parent = nullptr);
    void setImage(const QImage &img);
    void fit();
    void zoom100();

protected:
    void wheelEvent(QWheelEvent *e) override;
    void mouseDoubleClickEvent(QMouseEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;

private:
    void applyZoom(double f, const QPoint &at);
    QGraphicsScene *m_scene;
    QGraphicsPixmapItem *m_item = nullptr;
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
    void stop();
    void setDefaults(bool muted, bool loop, int volume);

private:
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

// Same-window post viewer: replaces the old popup dialog. Toolbar with
// back/prev/next + open-post/open-file actions, media area, metadata panel
// with clickable tags.
class ViewerPage : public QWidget {
    Q_OBJECT
public:
    explicit ViewerPage(QWidget *parent = nullptr);

    void setPosts(QVector<Post> posts);
    void showRow(int row);
    void setPlaybackDefaults(bool muted, bool loop, int volume);
    void setMaxSide(int px) { m_maxSide = qMax(512, px); }

signals:
    void back();
    void tagActivated(const QString &tag);

protected:
    void keyPressEvent(QKeyEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    bool eventFilter(QObject *watched, QEvent *e) override;

private:
    void showCurrent();
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
    QProgressBar *m_loadBar;
    QScrollArea *m_tagScroll = nullptr;
    QWidget *m_tagHost = nullptr;
    QBoxLayout *m_tagLayout = nullptr;
    int m_maxSide = 2560;
};
