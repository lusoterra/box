#pragma once

// Desktop integration for "Show in folder": open the file manager with the
// target file selected. The freedesktop FileManager1 D-Bus interface is
// tried first (Dolphin, Nautilus and friends implement it), then the
// known per-desktop `--select` switches, and finally the folder itself.
#include <QDesktopServices>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QUrl>

namespace DesktopFiles {

inline bool showInFolder(const QString &path) {
    const QFileInfo fi(path);
    if (!fi.exists())
        return false;
    const QString abs = fi.absoluteFilePath();
    const QString dir = fi.absolutePath();
    const QString url =
        QUrl::fromLocalFile(abs).toString(QUrl::FullyEncoded);

    // Standard interface: file managers open the parent window and select
    // the requested item (KDE/GNOME/XFCE implementations included).
    const QString dbusSend = QStandardPaths::findExecutable(
        QStringLiteral("dbus-send"));
    if (!dbusSend.isEmpty()) {
        QProcess p;
        p.start(dbusSend,
                {QStringLiteral("--session"),
                 QStringLiteral("--dest=org.freedesktop.FileManager1"),
                 QStringLiteral("--type=method_call"),
                 QStringLiteral("/org/freedesktop/FileManager1"),
                 QStringLiteral("org.freedesktop.FileManager1.ShowItems"),
                 QStringLiteral("array:string:") + url,
                 QStringLiteral("string:")});
        // dbus-send exits as soon as the call is delivered; the file
        // manager keeps running detached.
        if (p.waitForStarted(500) && p.waitForFinished(1800) &&
            p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0)
            return true;
        p.kill();
        p.waitForFinished(200);
    }

    struct Picker {
        const char *program;
        QStringList args;
    };
    const Picker pickers[] = {
        {"dolphin", {QStringLiteral("--select"), abs}},
        {"nautilus", {QStringLiteral("--select"), abs}},
        {"nemo", {QStringLiteral("--select"), abs}},
        {"caja", {QStringLiteral("--select"), abs}},
        {"thunar", {QStringLiteral("--select"), abs}},
    };
    for (const Picker &p : pickers) {
        const QString exe = QStandardPaths::findExecutable(
            QString::fromLatin1(p.program));
        if (exe.isEmpty())
            continue;
        if (QProcess::startDetached(exe, p.args))
            return true;
    }
    // Last resort: the folder without a selection.
    return QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
}

} // namespace DesktopFiles
