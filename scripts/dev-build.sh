#!/usr/bin/env bash
# Fast iteration build: no optimisation, no debug info beyond line tables,
# no test executables, Ninja + lld when available. Use `build`/the PKGBUILD
# for release artefacts.
#
#   scripts/dev-build.sh              # configure + build build-dev/box
#   JOBS=8 scripts/dev-build.sh       # use all cores for one run
#   scripts/dev-build.sh /tmp/box-dev # custom build directory
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
build=${1:-"$root/build-dev"}
jobs=${JOBS:-$(( $(nproc) / 2 ))}
ccache_flags=()
if command -v ccache >/dev/null 2>&1; then
    ccache_flags+=(-DCMAKE_C_COMPILER_LAUNCHER=ccache
                   -DCMAKE_CXX_COMPILER_LAUNCHER=ccache)
fi
link_flags=()
if command -v ld.lld >/dev/null 2>&1; then
    link_flags+=(-DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld)
fi

cmake -S "$root" -B "$build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_C_FLAGS_DEBUG="-O0 -g1" \
    -DCMAKE_CXX_FLAGS_DEBUG="-O0 -g1" \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DBUILD_TESTING=OFF \
    "${ccache_flags[@]}" "${link_flags[@]}"

# Only the app target: tests are a separate, much larger build and are
# configured out by BUILD_TESTING=OFF above.
cmake --build "$build" --target box -j "$jobs"
