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

// One gallery-dl flight downloads however many files the page holds.
// Box pins -D (exact directory), so gallery-dl writes straight into the
// _Stray root regardless of the user's folder templates; the system
// config still supplies credentials/cookies/proxy, but its
// post-processors and per-extractor folder/format choices are overridden,
// and archives/layered editor files (zip, 7z, psd, psb, …) are filtered
// out before download. Pixiv ugoira is the exception: gallery-dl fetches
// the original zip and box converts it with the bundled per-frame delays
// to an animated webp. Afterwards box flattens every new media file into
// the _Stray root using the normal stray naming (`_Stray_<md5>.<ext>`)
// and writes its own `_Stray_<md5>.json` (tags empty, post_url/source
// recorded, useful gallery-dl metadata such as file_url/title/artist
// carried over). Ignored leftovers are removed and nested folders pruned,
// so the media browser only ever sees flat strays.
// CLI for one download flight into destDir.
QStringList buildArgs(const QString &destDir, const QStringList &urls);

// <stem>.json sidecar path for a media file (LOSN layout, same as the
// stray importer writes).
QString sidecarPathFor(const QString &mediaPath);

// Write the user-owned stray sidecar for one media file when neither
// sidecar layout exists yet. postUrl is the pasted gallery page (the
// only attribution transport gives us). Returns false when a sidecar
// already exists or the write fails.
bool writeStraySidecar(const QString &mediaPath, const QString &postUrl);

// Flatten every media file under destDir not in `before` into the
// _Stray root (`_Stray_<md5>.<ext>` + matching JSON sidecar), carrying
// gallery-dl metadata over and deleting now-empty subfolders.
struct AdoptSummary {
    int files = 0;    // moved (or already-present duplicates dropped)
    int sidecars = 0; // new stray sidecars written
    int ugoira = 0;   // Pixiv ugoira zips converted to animated webp
};
AdoptSummary adoptNewFiles(const QString &destDir, const QSet<QString> &before,
                           const QString &postUrl);

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
