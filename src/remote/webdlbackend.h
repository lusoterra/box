#pragma once

// Browser-backed media download backend (QtCore types only, so plain
// network code can use it without linking WebEngine). The real
// implementation lives in cookiegrab.cpp (WebEngineDownloadBackend) and
// is injected by the app; null/absent means QNAM-only operation.
// Used for proof-of-work-walled hosts whose clearance is bound to the
// TLS fingerprint: no harvested jar authorizes plain requests there,
// while the capture profile passes transparently.
#include <QFile>
#include <QObject>
#include <QString>

#include <functional>

// True when a freshly downloaded file is actually a wall/login page
// (challenge HTML saved as .jpg): check both download paths, since a
// completed download is no proof of media bytes.
inline bool fileLooksLikeHtml(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return false;
    const QString head =
        QString::fromUtf8(f.read(2048)).trimmed().left(64).toLower();
    return head.startsWith(QStringLiteral("<!doctype")) ||
           head.startsWith(QStringLiteral("<html"));
}

class WebDownloadBackend : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;

public slots: // driven by DownloadManager on the backend's (GUI) thread
    virtual void start(qint64 id, const QString &url,
                       const QString &destPath) = 0;
    virtual void cancel(qint64 id) = 0;

public:
    // Blocking single fetch, callable from ANY thread (thumbnail pool
    // workers). Returns true with the bytes at destPath.
    virtual bool fetchBlocking(const QString &url, const QString &destPath,
                               QString *error = nullptr,
                               int timeoutMs = 60000) = 0;

signals:
    void progress(qint64 id, qint64 received, qint64 total);
    void finished(qint64 id, bool ok, const QString &error);
};
