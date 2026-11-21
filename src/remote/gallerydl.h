#pragma once

// System gallery-dl backend for gallery links (Pixiv/Fanbox/Pawchive,
// Kemono/Coomer, Patreon, Fantia, …). gallery-dl is tried first because
// it is built for exactly this: extractor coverage and login/OAuth
// handling (transport only - box writes its own user-owned sidecars).
// The built-in resolver/downloader stays the path
// for direct media URLs + booru post pages and the fallback when no
// system binary exists.
//
// Pure helpers (findBinary/available/version/handlesUrl/buildArgs) are
// unit-tested without touching the network; GalleryDlRunner owns one
// QProcess flight (log lines, progress, cancel, finish summary).
#include <QObject>
#include <QProcess>
#include <QSet>
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

// One gallery-dl flight downloads however many files the page holds
// (transport only: no --write-metadata, its tag blobs would pollute
// user tagging). Afterwards box writes its own stray sidecars for new
// files lacking one (<stem>.json, importer shape + post_url/source:
// tags empty for the user, never overwrites an existing sidecar).
// CLI for one download flight into destDir. The default filename
// templates stay (descriptive, unique-ish per post).
QStringList buildArgs(const QString &destDir, const QStringList &urls);

// <stem>.json sidecar path for a media file (LOSN layout, same as the
// stray importer writes).
QString sidecarPathFor(const QString &mediaPath);

// Write the user-owned stray sidecar for one media file when neither
// sidecar layout exists yet. postUrl is the pasted gallery page (the
// only attribution transport gives us). Returns false when a sidecar
// already exists or the write fails.
bool writeStraySidecar(const QString &mediaPath, const QString &postUrl);

// Sidecars for every media file under destDir not in `before`.
// Returns the number written.
int writeStraySidecars(const QString &destDir,
                       const QSet<QString> &before, const QString &postUrl);

} // namespace GalleryDl

// One gallery-dl download flight (async, UI thread). The process inherits
// the user's environment, so their gallery-dl config/login applies.
class GalleryDlRunner : public QObject {
    Q_OBJECT
public:
    struct Summary {
        bool ok = false;
        int filesFound = 0; // media files new under destDir
        int sidecarsWritten = 0;
        QString url; // pasted gallery page this flight downloaded
        QString error;
    };

    explicit GalleryDlRunner(const QString &binary,
                             const QString &destDir,
                             const QString &url, QObject *parent = nullptr);
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
    QSet<QString> mediaFiles() const;

    QString m_binary;
    QString m_destDir;
    QString m_url;
    QProcess *m_proc = nullptr;
    QSet<QString> m_filesBefore;
    bool m_cancelled = false;
};
