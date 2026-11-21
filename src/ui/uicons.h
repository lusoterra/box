#pragma once

// Lucide UI icons (ISC, see assets/icons/LICENSE.lucide), embedded via
// icons.qrc. Lucide strokes render black by default, so every icon is
// tinted for the dark palette at load.
#include <QColor>
#include <QHash>
#include <QIcon>
#include <QImage>
#include <QImageReader>
#include <QMutex>
#include <QMutexLocker>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QString>

namespace UiIcons {
inline QColor &normalRef() {
    static QColor c(0xd4, 0xd8, 0xf0);
    return c;
}
inline void setNormalColor(const QColor &c) {
    if (c.isValid())
        normalRef() = c;
}
inline QColor normal() {
    return normalRef();
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
    if (bucket == QStringLiteral("character"))
        return QColor(0x43, 0xd1, 0x7c); // green
    if (bucket == QStringLiteral("copyright"))
        return QColor(0xb5, 0x70, 0xe0); // purple (IP/copyright)
    if (bucket == QStringLiteral("meta"))
        return QColor(0xe0, 0x9a, 0x3c); // orange
    if (bucket == QStringLiteral("custom"))
        return QColor(0x7c, 0x9e, 0xf8); // native instance categories
    return QColor(0xe8, 0xe8, 0xec); // general: white
}

// Metatag colors for search-filter tokens (rating:g, sort:random,
// score:>10, ...): system operators get their own hues instead of the
// bucket white, so they read as operators at a glance. Invalid color
// when the token is a plain tag.
inline QColor filterTagColor(const QString &token) {
    QString t = token.trimmed().toLower();
    while (t.startsWith(u'-') || t.startsWith(u'~'))
        t = t.mid(1);
    const qsizetype colon = t.indexOf(u':');
    if (colon <= 0)
        return {};
    const QString key = t.left(colon);
    if (key == QStringLiteral("rating") || key == QStringLiteral("safety"))
        return QColor(0xff, 0x6b, 0x81); // rose
    if (key == QStringLiteral("sort") || key == QStringLiteral("order"))
        return QColor(0xb3, 0x88, 0xff); // violet
    if (key == QStringLiteral("score"))
        return QColor(0x69, 0xf0, 0xae); // mint
    if (key == QStringLiteral("id"))
        return QColor(0x82, 0xaa, 0xff); // light blue
    if (key == QStringLiteral("width") || key == QStringLiteral("height") ||
        key == QStringLiteral("mpixels") || key == QStringLiteral("filesize"))
        return QColor(0x4d, 0xd0, 0xe1); // cyan
    if (key == QStringLiteral("type") || key == QStringLiteral("filetype"))
        return QColor(0xff, 0xd5, 0x4f); // amber
    if (key == QStringLiteral("pool"))
        return QColor(0xce, 0x93, 0xd8); // lavender
    if (key == QStringLiteral("parent") || key == QStringLiteral("child"))
        return QColor(0xff, 0xab, 0x91); // peach
    if (key == QStringLiteral("user") || key == QStringLiteral("approver") ||
        key == QStringLiteral("commenter") || key == QStringLiteral("uploader"))
        return QColor(0x90, 0xca, 0xf9); // pale blue
    if (key == QStringLiteral("status") || key == QStringLiteral("flag") ||
        key == QStringLiteral("note") || key == QStringLiteral("appeal"))
        return QColor(0xef, 0x9a, 0x9a); // salmon
    if (key == QStringLiteral("tagcount") || key == QStringLiteral("favcount"))
        return QColor(0xbc, 0xaa, 0xa4); // taupe
    if (key == QStringLiteral("md5") || key == QStringLiteral("checksum"))
        return QColor(0xb0, 0xbe, 0xc5); // grey-blue
    if (key == QStringLiteral("source") || key == QStringLiteral("date") ||
        key == QStringLiteral("age") || key == QStringLiteral("created_at"))
        return QColor(0xff, 0xe0, 0x82); // sand
    if (key == QStringLiteral("limit") || key == QStringLiteral("page"))
        return QColor(0x9e, 0x9e, 0x9e); // grey
    return QColor(0x7c, 0x9e, 0xf8); // other prefix: accent
}

// Tinted Lucide art as a QImage. Worker threads (thumbnail placeholders)
// need this form: a QPixmap may only be built on the GUI thread. Cached by
// name/colour/size; thumb jobs render concurrently, hence the mutex.
inline QImage image(const QString &name, const QColor &color, int px) {
    static QMutex mutex;
    static QHash<QString, QImage> cache;
    const QString key = name + u'|' + color.name(QColor::HexArgb) + u'|' +
                        QString::number(px);
    {
        QMutexLocker lock(&mutex);
        const auto it = cache.constFind(key);
        if (it != cache.constEnd())
            return it.value();
    }
    QImage img;
    QImageReader reader(QStringLiteral(":/icons/ui/") + name +
                        QStringLiteral(".svg"));
    if (reader.canRead()) {
        const QSize src = reader.size();
        if (src.isValid())
            reader.setScaledSize(src.scaled(px, px, Qt::KeepAspectRatio));
        img = reader.read();
    }
    if (!img.isNull()) {
        img = img.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        QPainter p(&img);
        p.setCompositionMode(QPainter::CompositionMode_SourceIn);
        p.fillRect(img.rect(), color);
        p.end();
    }
    QMutexLocker lock(&mutex);
    cache.insert(key, img);
    return img;
}

inline QPixmap pixmap(const QString &name, const QColor &color, int px) {
    const QImage img = image(name, color, px);
    return img.isNull() ? QPixmap() : QPixmap::fromImage(img);
}

// Painted badges (grid marks, overview cards) draw the same art centred in
// a rect instead of relying on a font glyph.
inline void draw(QPainter *p, const QRect &rect, const QString &name,
                 const QColor &color) {
    const int px = qMax(1, qMin(rect.width(), rect.height()));
    const QImage img = image(name, color, px);
    if (img.isNull())
        return;
    QRect dst(QPoint(0, 0), img.size());
    dst.moveCenter(rect.center());
    p->drawImage(dst, img);
}

inline QIcon icon(const QString &name, const QColor &color, int px = 44) {
    const QPixmap pm = pixmap(name, color, px);
    return pm.isNull() ? QIcon() : QIcon(pm);
}

inline QIcon icon(const QString &name) {
    return icon(name, normal());
}

// Solid heart (Lucide only ships the outline): two lobes + a triangle,
// united with a winding fill. Used for every favourite mark.
inline QImage heartImage(const QColor &color, int px = 44) {
    QImage img(px, px, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    QPainter p(&img);
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
    return img;
}

inline QIcon heart(const QColor &color, bool filled, int px = 44) {
    if (!filled)
        return icon(QStringLiteral("heart"), color, px);
    return QIcon(QPixmap::fromImage(heartImage(color, px)));
}
} // namespace UiIcons
