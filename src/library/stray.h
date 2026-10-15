#pragma once

// Stray images: manually collected files with hand-written tags.
// They live in a fixed <downloadDir>/_Stray folder as _Stray_<md5>.<ext>
// with a native-shaped sidecar carrying a "stray" marker plus the flat
// tag blob (auto-categorized by the normal scan pipeline). Key rules:
// sidecar refresh never touches them, and the tag editor is only
// offered for them.
#include <QFileInfo>
#include <QString>

#include "post.h"

namespace Stray {
inline QString dirName() {
    return QStringLiteral("_Stray");
}
// A file is stray when it sits directly in a _Stray folder.
inline bool isStrayPath(const QString &absFile) {
    if (absFile.isEmpty())
        return false;
    return QFileInfo(absFile).dir().dirName() == dirName();
}
// Grid posts: local files under _Stray (remote posts never qualify).
inline bool isStrayPost(const Post &post) {
    return !post.isRemote && isStrayPath(post.absFile);
}
// Sidecar marker content.
inline QString markerKey() {
    return QStringLiteral("stray");
}
} // namespace Stray
