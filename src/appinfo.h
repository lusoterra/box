#pragma once

// Project identity: About box, welcome subtitle, user agents.
#include <QString>

inline QString appVersion() {
#ifdef BOX_VERSION
    return QStringLiteral(BOX_VERSION);
#else
    return QStringLiteral("dev");
#endif
}

inline QString appRepoUrl() {
    return QStringLiteral("https://git.lusosterone.net/lev/box");
}

inline QString appTagline() {
    return QStringLiteral("Booru client for local archives and online boorus");
}
