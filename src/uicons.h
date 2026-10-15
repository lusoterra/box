#pragma once

// Lucide UI icons (ISC, see assets/icons/LICENSE.lucide), embedded via
// icons.qrc. Lucide strokes render black by default, so every icon is
// tinted for the dark palette at load.
#include <QColor>
#include <QIcon>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QString>

namespace UiIcons {
inline QColor normal() {
    return QColor(0xd4, 0xd8, 0xf0);
}
inline QColor accent() {
    return QColor(0x7c, 0x9e, 0xf8);
}
inline QColor favRed() {
    return QColor(0xcc, 0x55, 0x55);
}

// Tag bucket colors (suggestions + filter pills share them).
inline QColor tagBucketColor(const QString &bucket) {
    if (bucket == QStringLiteral("artist"))
        return QColor(0xe0, 0x55, 0x61); // red
    if (bucket == QStringLiteral("copyright"))
        return QColor(0x43, 0xd1, 0x7c); // green
    if (bucket == QStringLiteral("character"))
        return QColor(0xe0, 0x9a, 0x3c); // orange
    if (bucket == QStringLiteral("meta"))
        return QColor(0xe5, 0xc5, 0x48); // yellow
    return QColor(0xe8, 0xe8, 0xec); // general: white
}

inline QIcon icon(const QString &name, const QColor &color, int px = 44) {
    QPixmap pm(QStringLiteral(":/icons/ui/") + name + QStringLiteral(".svg"));
    if (pm.isNull())
        return {};
    QPixmap scaled =
        pm.scaled(px, px, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    QPainter p(&scaled);
    p.setCompositionMode(QPainter::CompositionMode_SourceIn);
    p.fillRect(scaled.rect(), color);
    p.end();
    return QIcon(scaled);
}

inline QIcon icon(const QString &name) {
    return icon(name, normal());
}

// Solid heart (Lucide only ships the outline): two lobes + a triangle,
// united with a winding fill. Used for every favourite mark.
inline QIcon heart(const QColor &color, bool filled, int px = 44) {
    if (!filled)
        return icon(QStringLiteral("heart"), color, px);
    QPixmap pm(px, px);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(color);
    const double w = px, h = px;
    QPainterPath path;
    path.setFillRule(Qt::WindingFill);
    path.addEllipse(QRectF(w * 0.13, h * 0.10, w * 0.37, h * 0.37));
    path.addEllipse(QRectF(w * 0.50, h * 0.10, w * 0.37, h * 0.37));
    QPolygonF tri;
    tri << QPointF(w * 0.13, h * 0.30) << QPointF(w * 0.87, h * 0.30)
        << QPointF(w * 0.50, h * 0.90);
    path.addPolygon(tri);
    p.drawPath(path);
    p.end();
    return QIcon(pm);
}
} // namespace UiIcons
