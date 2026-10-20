#pragma once

#include <QImage>
#include <QList>
#include <QMutex>
#include <QObject>
#include <QRunnable>
#include <QSet>
#include <QString>
#include <QThreadPool>

// Async thumbnail loader on its OWN thread pool: sharing the global pool
// with the archive scanner starved thumbnails behind giant scans (and the
// loader's max-thread tweak leaked onto scan workers).
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
    // Browser backend for fingerprint-walled thumbnails (null = QNAM).
    // Set once by the app; read by pool tasks.
    void setWebBackend(class WebDownloadBackend *backend);

    // key: stable id for the request (caller-chosen). path/mtime/size
    // identify the file version. longSide: output pixel size. referer:
    // sent verbatim (hotlink-protected CDNs need it, e.g. Gelbooru).
    void request(const QString &key, const QString &path, qint64 mtime,
                 qint64 size, int longSide = 360, const QString &referer = {});
    // Drop everything queued (quit path): in-flight tasks are bounded by
    // their own network timeouts, but nothing new may start — otherwise
    // the pool destructor's waitForDone holds the close event open.
    void cancel();

signals:
    void thumbnailReady(const QString &key, const QImage &image);

private:
    friend class ThumbTask;
    void releaseKey(const QString &key); // task exit path (all outcomes)
    QThreadPool *m_pool;
    QSet<QString> m_inflight; // keys with a queued/running task (scroll dedupe)
    QMutex m_inflightMutex;
    WebDownloadBackend *m_webBackend = nullptr;
    int m_aspectNum = 1;
    int m_aspectDen = 1;
    QString m_cacheDir;
};

class ThumbTask : public QRunnable {
public:
    ThumbTask(ThumbLoader *loader, QString key, QString path, qint64 mtime,
              qint64 size, int longSide, QString referer = {});
    void run() override;

private:
    ThumbLoader *m_loader;
    QString m_key;
    QString m_path;
    QString m_referer;
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
    // Quit path: drop the queue and stop the player so neither late
    // frames nor the timeout can fire after the window is gone.
    void cancel();

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
    // Fresh object per job (see pump()): late frames from the previous
    // video arrive on the retired sink and are dropped by sender check,
    // so they can never become this job's thumbnail.
    QVideoSink *m_sink = nullptr;
    QTimer *m_timeout;
    int m_aspectNum = 1;
    int m_aspectDen = 1;
    QString m_cacheDir;
};
