#pragma once

// Fetch a remote URL into a durable temp file (TempLocation/box-ext) for
// consumers that need a real path: "Open externally" (mpv/qimgv can't
// play a URL for every build), and "Copy file" when no local file or
// media cache exists. This ignores the global media-cache policy on
// purpose: cache=nothing must not delete what the user is playing or
// pasting. Files are content-named (<sha1(url)>.ext) so repeat fetches
// of the same URL reuse them; the folder is wiped at the next app
// startup. Plain helper (function-local state only), no network-free
// test: verified via Xvfb.
#include <QObject>
#include <QString>
#include <functional>

namespace TempMedia {
// Fetch url to a stable temp path (extHint picks the extension when the
// URL has none or is unusable); onReady runs on the caller's thread with
// the path, or with an empty path on error. Repeated calls for the same
// URL share one flight and one file.
void ensureTemp(const QString &url, const QString &extHint,
                const std::function<void(const QString &)> &onReady);
// Temp folder used above (TempLocation/box-ext), created on demand.
QString tempDir();
} // namespace TempMedia
