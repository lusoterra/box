#include "applog.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QMutex>
#include <QMutexLocker>
#include <QtMessageHandler>

#include <cstdio>

namespace {
QMutex g_lock;
QFile g_file;
QString g_path;
qint64 g_maxBytes = 0;
int g_backups = 0;
qint64 g_size = 0; // bytes written to the current generation

// Harmless-but-chatty subsystems: their Debug/Info output is volume, not
// signal (hardware-decode probes, audio backends, Chromium internals).
// Matched case-insensitively; warnings and errors from these still land.
bool isSpam(const QString &msg) {
    static const char *kNoise[] = {
        "vdpau",       "vaapi",        "libva",      "webgpu",
        "dawn",        "swiftshader",  "pulseaudio", "pulse audio",
        "alsa",        "ffmpeg",       "libav",      "swscale",
        "swresample",  "avc",          "h264",       "hevc",
        "chromium",    "gpu process",  "angle",      "vulkan",
        "qt.webengine", "dbus",        "xkb",        "fcitx",
        "ibus",        "libinput",
    };
    for (const char *n : kNoise) {
        if (msg.contains(QString::fromLatin1(n), Qt::CaseInsensitive))
            return true;
    }
    return false;
}

void rotateLocked() {
    g_file.close();
    // box.log -> box.log.1 -> box.log.2 (oldest dropped).
    QFile::remove(g_path + QStringLiteral(".%1").arg(g_backups));
    for (int i = g_backups - 1; i >= 1; --i) {
        QFile::rename(g_path + QStringLiteral(".%1").arg(i),
                      g_path + QStringLiteral(".%1").arg(i + 1));
    }
    QFile::rename(g_path, g_path + QStringLiteral(".1"));
    g_file.setFileName(g_path);
    if (!g_file.open(QIODevice::WriteOnly | QIODevice::Append |
                     QIODevice::Text))
        return; // read-only dir etc: stderr logging continues
    g_size = g_file.size();
}

void handler(QtMsgType type, const QMessageLogContext &ctx,
             const QString &msg) {
    if (msg.trimmed().isEmpty())
        return;
    const char *level = nullptr;
    bool noisy = false;
    switch (type) {
    case QtDebugMsg:
        level = "DEBUG";
        noisy = true;
        break;
    case QtInfoMsg:
        level = "INFO";
        noisy = true;
        break;
    case QtWarningMsg:
        level = "WARN";
        break;
    case QtCriticalMsg:
        level = "ERROR";
        break;
    case QtFatalMsg:
        level = "FATAL";
        break;
    }
    if (noisy && isSpam(msg))
        return; // volume, not signal
    QString line = QStringLiteral("[%1 %2%3] %4\n")
                       .arg(QDateTime::currentDateTime().toString(
                           QStringLiteral("hh:mm:ss")))
                       .arg(QString::fromLatin1(level))
                       .arg(ctx.category ? QStringLiteral(" ") +
                                               QString::fromLatin1(ctx.category)
                                         : QString())
                       .arg(msg.trimmed());
    const QByteArray bytes = line.toUtf8();
    fputs(bytes.constData(), stderr);
    QMutexLocker lock(&g_lock);
    if (g_file.isOpen()) {
        if (g_maxBytes > 0 && g_size + bytes.size() > g_maxBytes)
            rotateLocked();
        if (g_file.isOpen()) {
            g_file.write(bytes);
            g_file.flush();
            g_size += bytes.size();
        }
    }
    if (type == QtFatalMsg)
        abort();
}
} // namespace

namespace AppLog {
void install(const QString &filePath, qint64 maxBytes, int backups) {
    QMutexLocker lock(&g_lock);
    g_path = filePath;
    g_maxBytes = maxBytes;
    g_backups = qMax(1, backups);
    QDir().mkpath(QFileInfo(filePath).absolutePath());
    g_file.setFileName(filePath);
    if (!g_file.open(QIODevice::WriteOnly | QIODevice::Append |
                     QIODevice::Text))
        g_file.setFileName(QString()); // stderr only from here
    g_size = g_file.size();
    lock.unlock();
    qInstallMessageHandler(handler);
}
} // namespace AppLog
