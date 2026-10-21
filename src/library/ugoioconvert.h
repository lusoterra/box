#pragma once

// Ugoira (animated illustration archives, served as .zip) download
// conversion: unzip frames, encode to animated WebP or JPEG XL via a
// system ffmpeg (+cjxl for JXL), at the post's per-frame delays (or the
// configured default when the API omits pixiv frame data). Output is
// never GIF (poor colouring/dithering on gradients).
#include <QAtomicInt>
#include <QImage>
#include <QJsonObject>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QVector>

namespace Ugoira {
// Decoded frames in archive order, with per-frame delays in ms.
struct Frames {
    QVector<QImage> images;
    QStringList names; // parallel to images, for delay matching
    QVector<int> delaysMs;
    bool isEmpty() const { return images.isEmpty(); }
};
// pixiv_ugoira_frame_data tolerant parse: {data:[{file|name, delay|delay_ms?}]}.
// Units are milliseconds; garbage falls back to defaultMs.
QMap<QString, int> delaysFromRaw(const QJsonObject &raw, int defaultMs);
// Zip -> decoded frames (miniz). Caps entries and total bytes against
// hostile archives. Delays attach by basename, default-filled.
bool unzipFrames(const QString &zipPath, Frames &frames,
                 const QMap<QString, int> &delaysByName, int defaultDelayMs,
                 QString *error);
// Frames (+delays) -> animated file. WebP goes straight through ffmpeg;
// JXL goes via an APNG intermediate into cjxl. Runs encoders as child
// processes with event-loop pumping; *cancel (nullable) aborts between
// slices and kills the child. Requires ffmpeg in PATH (+cjxl for jxl).
bool encodeAnimated(const Frames &frames, const QString &format,
                    const QString &outPath, const QAtomicInt *cancel,
                    QString *error);
} // namespace Ugoira
