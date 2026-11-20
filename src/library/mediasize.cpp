#include "mediasize.h"

#include "post.h"

#include <QFileInfo>
#include <QImageReader>
#include <QSize>

namespace MediaSize {

bool probeStill(const QString &path, const QString &ext, int &width,
                int &height) {
    if (width > 0 && height > 0)
        return true;
    if (path.isEmpty() || isVideoExt(ext))
        return false;
    const QFileInfo fi(path);
    if (!fi.isFile())
        return false;
    // Header-only read: QImageReader::size() never decodes the pixels.
    QImageReader reader(path);
    reader.setAutoTransform(false); // stored dimensions, not display ones
    const QSize size = reader.size();
    if (!size.isValid() || size.isEmpty())
        return false;
    width = size.width();
    height = size.height();
    return true;
}

bool probeFile(const QString &path, int &width, int &height) {
    if (width > 0 && height > 0)
        return true;
    if (path.isEmpty())
        return false;
    const QFileInfo fi(path);
    if (!fi.isFile())
        return false;
    return probeStill(path, fi.suffix().toLower(), width, height);
}

} // namespace MediaSize
