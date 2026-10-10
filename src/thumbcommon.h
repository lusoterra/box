#pragma once

// Shared thumbnail shaping: center-crop to a uniform aspect, scale the
// longest side down, painted fallback for undecodable videos.
#include <QColor>
#include <QFont>
#include <QImage>
#include <QPainter>
#include <QString>

namespace ThumbCommon {

// Center-crop to aspect `want` (w/h), then scale longest side to `longSide`.
inline QImage cropScale(QImage img, double want, int longSide) {
    if (img.isNull())
        return img;
    const double have = double(img.width()) / double(qMax(1, img.height()));
    if (have > want) {
        const int w = qMax(1, int(img.height() * want));
        img = img.copy((img.width() - w) / 2, 0, w, img.height());
    } else if (have < want) {
        const int h = qMax(1, int(img.width() / want));
        img = img.copy(0, (img.height() - h) / 2, img.width(), h);
    }
    if (qMax(img.width(), img.height()) > longSide) {
        img = img.scaled(longSide, int(longSide / want) + 1, Qt::KeepAspectRatio,
                         Qt::SmoothTransformation);
        const double now = double(img.width()) / double(qMax(1, img.height()));
        if (qAbs(now - want) > 0.02) {
            if (now > want) {
                const int w = qMax(1, int(img.height() * want));
                img = img.copy((img.width() - w) / 2, 0, w, img.height());
            } else {
                const int h = qMax(1, int(img.width() / want));
                img = img.copy(0, (img.height() - h) / 2, img.width(), h);
            }
        }
    }
    return img;
}

inline QSize targetSize(double want, int longSide) {
    if (want >= 1.0)
        return {longSide, qMax(1, int(longSide / want))};
    return {qMax(1, int(longSide * want)), longSide};
}

// Painted placeholder used when a video yields no decodable frame.
inline QImage videoPlaceholder(const QString &ext, double want, int longSide) {
    const QSize ts = targetSize(want, longSide);
    QImage img(ts, QImage::Format_ARGB32);
    img.fill(QColor(0x1e, 0x21, 0x30));
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QColor(0x7c, 0x9e, 0xf8));
    QFont f = p.font();
    f.setPixelSize(qMin(ts.width(), ts.height()) / 3);
    p.setFont(f);
    p.drawText(img.rect(), Qt::AlignCenter, QStringLiteral("▶"));
    f.setPixelSize(14);
    p.setFont(f);
    p.setPen(QColor(0x78, 0x80, 0xa0));
    p.drawText(img.rect().adjusted(0, 0, -8, -8), Qt::AlignRight | Qt::AlignBottom,
               ext.toUpper());
    return img;
}

} // namespace ThumbCommon
