#pragma once

// Perceptual hashing for duplicate detection. Files are decoded to a
// normalized 32x32 grayscale image and reduced to a 64-bit DCT hash
// (pHash): equal or resized/re-encoded copies stay close in Hamming
// distance, while metadata (tags, sidecars, format) never enters the
// hash. Lossy JPEG recompression can cross the threshold; exact SHA-256
// copies are always matched separately.
#include <QImage>
#include <QString>
#include <QtGlobal>

struct PixelHash {
    quint64 hash = 0;
    int width = 0;
    int height = 0;
    bool ok = false;
    // True when the decoder reports an animation (GIF, animated WebP).
    // Only the first frame is hashed; the flag keeps animated files in a
    // separate duplicate pool from stills.
    bool animated = false;
};

// Bit distance between two pHashes (0..64).
int phashDistance(quint64 a, quint64 b);

// 64-bit pHash of an already decoded image. Pure; used by tests.
quint64 perceptualHashImage(const QImage &image);

// Decode the first frame (or the still) and hash it. `ok` stays false
// when the format is unsupported or the file cannot be decoded.
PixelHash perceptualHashFile(const QString &path);
