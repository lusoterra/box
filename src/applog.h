#pragma once

// Central log sink. Installs a Qt message handler that condenses Qt/ffmpeg
// chatter into timestamped ERROR/WARN/INFO lines on stderr and mirrors them
// into a rotating log file (fixed name under the data dir, 128 MB default
// cap, older generations kept as .1/.2/.3). Spammy but harmless Debug/Info
// lines (VAAPI/VDPAU/WebGPU/pulse/ffmpeg internals) are dropped; warnings
// and errors always land, since those are what diagnose crashes.
#include <QString>

namespace AppLog {
// maxBytes <= 0 disables the file (stderr only). backups = rotated copies.
void install(const QString &filePath,
             qint64 maxBytes = 128LL * 1024 * 1024, int backups = 3);
} // namespace AppLog
