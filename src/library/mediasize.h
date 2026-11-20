#pragma once

// Local media size probe: pixel dimensions read straight from a file that
// is already on disk. No network, no full decode, no sidecar rewrite.
//
// Stills go through QImageReader::size(), which parses only the header
// (PNG IHDR, JPEG SOF, ...). Videos need a media player and are left to
// the caller: probing them here would mean spinning up QMediaPlayer and
// waiting asynchronously, which does not belong in a synchronous export.
#include <QString>

namespace MediaSize {

// Fills width/height (only when both are still unknown) from `path`.
// Returns true when the file answered. Silent on missing/unreadable files.
bool probeFile(const QString &path, int &width, int &height);

// Same, for an extension that is known to be a still image (gif/webp/jxl
// included). `ext` is lowercase and dot-less; videos are refused.
bool probeStill(const QString &path, const QString &ext, int &width,
                int &height);

} // namespace MediaSize
