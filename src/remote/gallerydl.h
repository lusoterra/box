#pragma once

// System gallery-dl backend for gallery links (Pixiv/Fanbox/Pawchive,
// Kemono/Coomer, Patreon, Fantia, …). gallery-dl is tried first because
// it is built for exactly this: extractor coverage, login/OAuth handling
// and metadata sidecars. The built-in resolver/downloader stays the path
// for direct media URLs + booru post pages and the fallback when no
// system binary exists.
//
// Pure helpers (findBinary/available/version/handlesUrl/buildArgs) are
// unit-tested without touching the network; GalleryDlRunner owns one
// QProcess flight (log lines, progress, cancel, finish summary).
#include <QObject>
#include <QProcess>
#include <QString>
#include <QStringList>

namespace GalleryDl {

// Binary lookup (PATH). Empty when not installed.
QString findBinary();
bool available();

// "1.32.20" from `gallery-dl --version` output; empty on failure.
// The pure overload parses captured output (unit-tested).
QString version(const QString &binary);
QString parseVersion(const QString &output);

// True when the URL is a gallery/post page for an extractor gallery-dl
// covers AND this app should hand it over: not a direct media file
// (those stay on the built-in downloader) and not a recognised booru
// post page (the native resolver yields better sidecars there).
bool handlesUrl(const QString &url);

// CLI for one download flight into destDir. --write-metadata gives the
// per-file `<media>.json` sidecars the scanner already parses; the
// default filename templates stay (descriptive, unique-ish per post).
QStringList buildArgs(const QString &destDir, const QStringList &urls);

} // namespace GalleryDl

// One gallery-dl download flight (async, UI thread). The process inherits
// the user's environment, so their gallery-dl config/login applies.
class GalleryDlRunner : public QObject {
    Q_OBJECT
public:
    struct Summary {
        bool ok = false;
        int filesFound = 0; // media files new under destDir
        QString error;
    };

    explicit GalleryDlRunner(const QString &binary,
                             const QString &destDir,
                             const QStringList &urls, QObject *parent = nullptr);
    void start();
    void cancel();
    bool isActive() const { return m_proc != nullptr; }

signals:
    void logLine(const QString &line);
    void progress(const QString &current);
    void finished(const Summary &summary);

private slots:
    void onReadyRead();
    void onProcessFinished(int exitCode, QProcess::ExitStatus status);

private:
    int countMediaFiles() const;

    QString m_binary;
    QString m_destDir;
    QStringList m_urls;
    QProcess *m_proc = nullptr;
    int m_filesBefore = 0;
    bool m_cancelled = false;
};
