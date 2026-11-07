#!/usr/bin/env bash
# Incremental build + system install. Skips compiling when ./build/box is
# already up to date and just copies the binary over.
set -euo pipefail

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
BUILD=${BOX_BUILD_DIR:-"$ROOT/build"}
PREFIX=${BOX_PREFIX:-/usr}
JOBS=${JOBS:-$(nproc 2>/dev/null || echo 4)}

SUDO=()
if [[ ${EUID:-$(id -u)} -ne 0 ]]; then
	SUDO=(sudo)
fi

if [[ -x "$BUILD/box" ]]; then
	# Skip compile if binary is newer than sources/build files.
	need_build=0
	if find "$ROOT/src" "$ROOT/CMakeLists.txt" -type f -newer "$BUILD/box" | grep -q .; then
		need_build=1
	fi
	if [[ "$need_build" == "0" ]]; then
		echo "== box: $BUILD/box is up to date, skipping compile"
	else
		echo "== box: sources newer, rebuilding"
		cmake -B "$BUILD" -S "$ROOT" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" >/dev/null
		cmake --build "$BUILD" -j"$JOBS"
	fi
else
	echo "== box: building $ROOT -> $BUILD"
	cmake -B "$BUILD" -S "$ROOT" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" >/dev/null
	cmake --build "$BUILD" -j"$JOBS"
fi

echo "== box: installing binary to $PREFIX"
"${SUDO[@]}" install -Dm755 "$BUILD/box" "$PREFIX/bin/box"

# desktop caches best effort
if [[ -d "$PREFIX/share/applications" ]]; then
	"${SUDO[@]}" update-desktop-database "$PREFIX/share/applications" >/dev/null 2>&1 || true
fi
hash -r 2>/dev/null || true
echo "== box: installed -> $PREFIX/bin/box"
