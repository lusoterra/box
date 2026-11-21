#pragma once

#include "boorutypes.h"

// Per-family engine singletons live in engine_<family>.cpp. The switch in
// booruengine.cpp and every family file include this header so both sides
// agree on the declarations (-Wmissing-declarations).
namespace EngineInternal {

const BooruEngineOps *danbooruOps();
const BooruEngineOps *gelbooruOps();
const BooruEngineOps *moebooruOps();
const BooruEngineOps *szurubooruOps();
const BooruEngineOps *shimmieOps();
const BooruEngineOps *hybooruOps();
const BooruEngineOps *nuuruOps();
const BooruEngineOps *philomenaOps();
const BooruEngineOps *booruOnRailsOps();
const BooruEngineOps *e621Ops();
const BooruEngineOps *animePicturesOps();

} // namespace EngineInternal
