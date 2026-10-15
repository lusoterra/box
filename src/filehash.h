#pragma once

// Content hashing for local files. Booru APIs index posts by the md5 of
// the file bytes, so a locally computed md5 doubles as a lookup key
// (md5:<hash> search): files whose names carry no md5 (Site_losn123,
// Site_456) get identified by hashing instead of guessing from URLs.
#include <QCryptographicHash>
#include <QFile>
#include <QString>

// md5 of the whole file, 32 lowercase hex digits. Empty when the file
// cannot be read. Chunked: multi-hundred-MB videos never fully resident.
inline QString fileContentMd5(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    QCryptographicHash h(QCryptographicHash::Md5);
    while (!f.atEnd())
        h.addData(f.read(1 << 20));
    return QString::fromLatin1(h.result().toHex());
}

// True when a filename stem already carries a content hash
// (<Site>_<32 hex>), i.e. nothing to fix up.
inline bool isMd5Stem(const QString &stem) {
    if (stem.size() < 34)
        return false;
    const QString tail = stem.right(32);
    for (const QChar &c : tail) {
        const ushort u = c.unicode();
        const bool hex = (u >= u'0' && u <= u'9') || (u >= u'a' && u <= u'f') ||
                         (u >= u'A' && u <= u'F');
        if (!hex)
            return false;
    }
    return stem.at(stem.size() - 33) == u'_';
}
