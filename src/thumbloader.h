#pragma once

#include <QImage>
#include <QList>
#include <QObject>
#include <QRunnable>
#include <QSet>
#include <QString>
#include <QThreadPool>

// Async thumbnail loader backed by the global QThreadPool (max 6 workers).
// Thumbnails are cropped center-fill to a uniform aspect ratio (1:1, 4:3,
// custom…) so every grid entry has identical geometry, and persisted to a
// disk cache keyed by (path, mtime, size, aspect) so large archives only
// decode once. Results come back via thumbnailReady(key, image).
class ThumbLoader : public QObject {
    Q_OBJECT
public:
    explicit ThumbLoader(QObject *parent = nullptr);

    void setAspect(int num, int den); // e.g. 1,1 or 4,3
    void setCacheDir(const QString &dir);
    void setMaxThreads(int n);

    // key: stable id for the request (caller-chosen). path/mtime/size
    // identify the file version. longSide: output pixel size.
    void request(const QString &key, const QString &path, qint64 mtime,
                 qint64 size, int longSide = 360);

signals:
    void thumbnailReady(const QString &key, const QImage &image);

private:
    friend class ThumbTask;
    QThreadPool *m_pool;
    int m_aspectNum = 1;
    int m_aspectDen = 1;
    QString m_cacheDir;
};

class ThumbTask : public QRunnable {
public:
    ThumbTask(ThumbLoader *loader, QString key, QString path, qint64 mtime,
              qint64 size, int longSide);
    void run() override;

private:
    ThumbLoader *m_loader;
    QString m_key;
    QString m_path;
    qint64 m_mtime;
    qint64 m_size;
    int m_longSide;
};

class QMediaPlayer;
class QTimer;
class QVideoSink;

// First-frame extractor for videos. QMediaPlayer is unreliable in thread-pool
// threads, so this lives in the GUI thread and works through a sequential
// queue (videos are rare enough that one-at-a-time is plenty). Same
// thumbnailReady(key, image) contract and same disk cache as ThumbLoader.
class VideoThumbGrabber : public QObject {
    Q_OBJECT
public:
    explicit VideoThumbGrabber(QObject *parent = nullptr);

    void setAspect(int num, int den);
    void setCacheDir(const QString &dir);
    void request(const QString &key, const QString &path, qint64 mtime,
                 qint64 size, int longSide = 360);

signals:
    void thumbnailReady(const QString &key, const QImage &image);

private:
    void pump();
    void finishCurrent(const QImage &img);

    struct Job {
        QString key;
        QString path;
        qint64 mtime = 0;
        qint64 size = 0;
        int longSide = 360;
    };
    QList<Job> m_queue;
    QSet<QString> m_queued; // keys already waiting (de-dupe scroll spam)
    bool m_busy = false;
    Job m_current;
    QString m_diskPath;
    double m_want = 1.0;
    QMediaPlayer *m_player;
    QVideoSink *m_sink;
    QTimer *m_timeout;
    int m_aspectNum = 1;
    int m_aspectDen = 1;
    QString m_cacheDir;
};
