#!/usr/bin/env bash
# Incremental build + system install, so the GUI always launches the latest
# binary regardless of how an older copy was installed.
#
#   ./scripts/install-latest.sh            # build and install to /usr
#   BOX_PREFIX=/usr/local ...              # another prefix
#   BOX_BUILD_DIR=...                      # reuse another build tree
#
# The build tree is incremental (no clean): re-run after edits. Any stale
# manual boxes under /usr/local/bin or ~/.local/bin are removed because they
# shadow the package-installed /usr/bin/box in PATH.
set -euo pipefail

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
BUILD=${BOX_BUILD_DIR:-"$ROOT/build-install"}
PREFIX=${BOX_PREFIX:-/usr}
JOBS=${JOBS:-$(nproc 2>/dev/null || echo 4)}

SUDO=()
if [[ ${EUID:-$(id -u)} -ne 0 ]]; then
	SUDO=(sudo)
fi

echo "== box: building $ROOT (incremental) -> $BUILD"
cmake -B "$BUILD" -S "$ROOT" \
	-DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_INSTALL_PREFIX="$PREFIX" >/dev/null
cmake --build "$BUILD" -j"$JOBS"

echo "== box: installing to $PREFIX"
"${SUDO[@]}" cmake --install "$BUILD"

# Old manual installs win over /usr/bin in the default PATH/icon lookup.
# Remove only the exact box/booru-explorer files; never unrelated files.
for stale in \
	/usr/local/bin/box \
	"$HOME/.local/bin/box" \
	/usr/local/share/applications/box.desktop \
	"$HOME/.local/share/applications/box.desktop" \
	/usr/local/share/icons/hicolor/512x512/apps/booru-explorer.png \
	/usr/local/share/icons/hicolor/scalable/apps/booru-explorer.svg \
	"$HOME/.local/share/icons/hicolor/512x512/apps/booru-explorer.png" \
	"$HOME/.local/share/icons/hicolor/scalable/apps/booru-explorer.svg"; do
	[[ "$stale" == "$PREFIX"/* ]] && continue
	if [[ -e "$stale" ]]; then
		echo "== box: removing stale $stale"
		"${SUDO[@]}" rm -f -- "$stale"
	fi
done

# Desktop caches (best effort; harmless when the tools are absent).
if [[ -d "$PREFIX/share/applications" ]]; then
	"${SUDO[@]}" update-desktop-database "$PREFIX/share/applications" \
		>/dev/null 2>&1 || true
fi
if [[ -d "$PREFIX/share/icons/hicolor" ]]; then
	"${SUDO[@]}" gtk-update-icon-cache -q -t -f \
		"$PREFIX/share/icons/hicolor" >/dev/null 2>&1 || true
fi

hash -r 2>/dev/null || true
echo "== box: installed -> $(command -v box || echo "$PREFIX/bin/box")"
