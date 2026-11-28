#pragma once

// Booru engine auto-detection from a base URL. Pure aside from the
// injected fetch callback, so the probe order and signatures are
// unit-testable without network access.
#include "boorutypes.h"

#include <QByteArray>
#include <QString>

#include <functional>

namespace EngineDetect {

struct Result {
    BooruEngine engine = BooruEngine::Danbooru;
    QString baseUrl; // may carry a discovered subpath (Szurubooru /booru)
    bool found = false;
};

using Fetch = std::function<QByteArray(const QString &url)>;

// Probe the known API shapes (e-shuushuu, Shimmie XML, Szurubooru,
// e621, Danbooru, AnimePictures, HyBooru, Philomena, BooruOnRails,
// Moebooru, Nuuru, Gelbooru). Common public hosts resolve from an
// embedded hostname table before any request. `fetch` must return an
// empty body on failure; never throws. found=false when nothing matches.
Result detect(const QString &base, const Fetch &fetch);

} // namespace EngineDetect
