#include "phash.h"

#include <QImageReader>

#include <algorithm>
#include <bit>
#include <cmath>
#include <numbers>
#include <vector>

namespace {

constexpr int kSize = 32; // decoded grid
constexpr int kLow = 8;   // low-frequency DCT block

// cos((2x+1)u*pi/(2N)) for the separable 32-point DCT.
struct CosineTable {
    double c[kSize][kSize];
    CosineTable() {
        for (int u = 0; u < kSize; ++u)
            for (int x = 0; x < kSize; ++x)
                c[u][x] = std::cos((2.0 * x + 1.0) * u * std::numbers::pi /
                                   (2.0 * kSize));
    }
};

const CosineTable &cosines() {
    static const CosineTable table;
    return table;
}

double alpha(int u) {
    return u == 0 ? std::sqrt(1.0 / kSize) : std::sqrt(2.0 / kSize);
}

} // namespace

int phashDistance(quint64 a, quint64 b) {
    return std::popcount(a ^ b);
}

quint64 perceptualHashImage(const QImage &image) {
    if (image.isNull())
        return 0;
    QImage img = image;
    if (img.size() != QSize(kSize, kSize))
        img = img.scaled(kSize, kSize, Qt::IgnoreAspectRatio,
                         Qt::SmoothTransformation);
    img = img.convertToFormat(QImage::Format_Grayscale8);
    if (img.isNull())
        return 0;

    const CosineTable &ct = cosines();
    // Row pass: rowDct[y][u] = sum_x pixel(x,y) * cos((2x+1)u*pi/64).
    double rowDct[kSize][kSize] = {};
    for (int y = 0; y < kSize; ++y) {
        const uchar *line = img.constScanLine(y);
        for (int u = 0; u < kLow; ++u) {
            double sum = 0.0;
            for (int x = 0; x < kSize; ++x)
                sum += static_cast<double>(line[x]) * ct.c[u][x];
            rowDct[y][u] = sum;
        }
    }
    // Column pass for the top-left 8x8 block.
    double coeff[kLow][kLow] = {};
    for (int v = 0; v < kLow; ++v) {
        for (int u = 0; u < kLow; ++u) {
            double sum = 0.0;
            for (int y = 0; y < kSize; ++y)
                sum += rowDct[y][u] * ct.c[v][y];
            coeff[v][u] = alpha(u) * alpha(v) * sum;
        }
    }

    // Median of the 63 AC coefficients (DC excluded: brightness-only
    // changes must not move every bit).
    std::vector<double> ac;
    ac.reserve(kLow * kLow - 1);
    for (int v = 0; v < kLow; ++v)
        for (int u = 0; u < kLow; ++u)
            if (u != 0 || v != 0)
                ac.push_back(coeff[v][u]);
    std::nth_element(ac.begin(), ac.begin() + ac.size() / 2, ac.end());
    const double median = ac[ac.size() / 2];

    quint64 hash = 0;
    int bit = 0;
    for (int v = 0; v < kLow; ++v) {
        for (int u = 0; u < kLow; ++u) {
            if (u == 0 && v == 0)
                continue;
            if (coeff[v][u] > median)
                hash |= (quint64(1) << bit);
            ++bit;
        }
    }
    return hash;
}

PixelHash perceptualHashFile(const QString &path) {
    PixelHash out;
    QImageReader reader(path);
    reader.setAutoTransform(true);
    const QSize orig = reader.size();
    if (orig.isValid()) {
        out.width = orig.width();
        out.height = orig.height();
    }
    out.animated = reader.supportsAnimation();
    // Ask the decoder to scale while decoding (JPEG especially); readers
    // that ignore it still decode normally below.
    if (orig.isValid() && (orig.width() > kSize || orig.height() > kSize))
        reader.setScaledSize(QSize(kSize, kSize));
    QImage img = reader.read();
    if (img.isNull())
        return out;
    if (out.width <= 0 || out.height <= 0) {
        out.width = img.width();
        out.height = img.height();
    }
    out.hash = perceptualHashImage(img);
    out.ok = true;
    return out;
}
