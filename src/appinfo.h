#pragma once

// Project identity: About box, welcome subtitle, user agents.
#include <QString>

inline QString appVersion() {
#ifdef BOORU_EXPLORER_VERSION
    return QStringLiteral(BOORU_EXPLORER_VERSION);
#else
    return QStringLiteral("dev");
#endif
}

inline QString appRepoUrl() {
    return QStringLiteral("https://git.lusosterone.net/lev/booru-explorer");
}

inline QString appTagline() {
    return QStringLiteral("Booru client for local archives and online boorus");
}
