# TESTING - box

## Build / test

- Configure + compile: `cmake -B build -DCMAKE_BUILD_TYPE=Release` then
  `cmake --build build -j"$(nproc)"` -> `build/box`.
- Tests: `ctest --test-dir build --output-on-failure`; single case:
  `ctest --test-dir build -R <name>`.
- Run from the build tree: `./build/box [ROOT]`.
- Install: `cmake --install build` (prefix set at configure time);
  uninstall by removing the paths listed in `build/install_manifest.txt`.
- Do NOT build in doc-writing sessions unless asked (read-only + docs).

## Fast dev builds

- `scripts/dev-build.sh` configures `build-dev/` as Debug with
  `-O0 -g1`, Ninja + `ld.lld` when installed, `BUILD_TESTING=OFF`, and
  builds only the `box` target. No optimisation flags leak into the
  release/PKGBUILD build.
- Rebuild after a source edit:
  `cmake --build build-dev --target box -j"$(( $(nproc) / 2 ))"` (or
  `ninja -C build-dev -j4 box`). A no-op build is instant; a single
  `mainwindow.cpp` rebuild is ~20 s instead of ~38 s at `-O3`.
- `JOBS=8 scripts/dev-build.sh` uses all cores for one run if you do not
  need the half-core default.
- Install `ccache` and rerun the script to enable the compiler launcher;
  clean rebuilds and branch switches then reuse object files.
- `scripts/install-latest.sh` defaults to `build/` + CMake `Release` and
  builds only the `box` target (tests are not needed to install). Override
  with `BOX_BUILD_TYPE=Debug`, `BOX_BUILD_DIR=build-dev`, `BOX_PREFIX=…`.
- Fatal signals write a backtrace to `box-crash.log` next to `box.log`
  (`~/.local/share/box/box-crash.log` by default), then re-raise so
  `coredumpctl` still captures a core. Attach it to crash reports.
- Tests live in the normal `build/` tree. To use tests in the dev tree,
  reconfigure it without `-DBUILD_TESTING=OFF` (or run
  `cmake --build build-dev --target test_favdb` after re-enabling).

`test_dedup` covers the pHash/JPEG tolerance, family/type grouping,
`dedup.db` caching/pruning and a real archive scan; the GUI suite opens
the dialog, scans exact copies, and exercises archive + Hydrus deletion
(trash, sidecars, hydrus.db row).

## First-launch offline check

- `test_gridwindow::firstLaunchStaysOffline` builds a `MainWindow` with
  fresh `XDG_*` temp dirs, waits, and asserts `NetworkPolicy::allowed()`
  is still false, no `webprofile` exists and no favicon was cached.
- For an end-to-end check, preload a tiny `connect()`/`getaddrinfo()`
  logger and run an offscreen build with empty XDG dirs for ~10 s: the
  log must stay empty. Example probe source lives in
  `/tmp/opencode/netprobe/probe.c` during a manual session; rebuild with
  `gcc -shared -fPIC -o probe.so probe.c -ldl` and set
  `LD_PRELOAD=$PWD/probe.so CONNLOG=$PWD/conn.log`.
- `test_settings` covers the `classify/backend|auto|custom*` round-trip
  and the `hydrus/tagLookups|tab` split with its legacy `hydrus/enabled`
  migration;
  `test_tagtypes` proves off/hydrus never fetch and that
  gelbooru-without-credentials and a bad custom URL report an error
  instead of falling back; `test_hydrusstore` checks the local
  namespace dictionary; `test_gridwindow::tagsPreferencesBuild` opens
  the Tags page and the backend combo.

## e-shuushuu + engine detection

- `test_shuushuu` covers the image mapper (tag types, md5, URLs), the
  tag-ID search URL, tag-name/alias resolution and a full two-step
  preflight against a local HTTP server (`/api/v1/tags` then
  `/api/v1/images`).
- `test_enginedetect` feeds fake probe bodies for every engine and checks
  the order/signatures (e-shuushuu, e621, Danbooru, AnimePictures,
  HyBooru, Philomena, Booru-on-Rails, Shimmie, Szurubooru, Moebooru,
  Gelbooru, Nuuru), the known-hostname fast path (zero fetches, scheme-
  less input, Szuru `/booru` subpath) plus the unknown-host case.

## Box Share web UI

- `test_shareserver::rootServesTheSharePage` checks `/` and `/index.html`
  return HTML and that the embedded `:/share/index.html` resource is
  present when `web/dist/index.html` was embedded.
- The file is committed as a single built page; after frontend changes run
  `npm ci && npm run build` in `web/` and commit `web/dist/index.html`.
  CMake prints a status line and falls back to the plain API page when the
  file is missing, so a source tree without it still builds.
- `test_gridwindow` starts the server from the dialog and checks the red
  `LIVE` badge and its web/API tooltip.

## Test layout

- One binary per file: `add_unit_test(name sources...)` in
  `tests/CMakeLists.txt:5-16`; each `add_unit_test(test_X test_X.cpp ...)`
  = one `ctest` case (`tests/CMakeLists.txt:30-70`).
- Conventions (`tests/CMakeLists.txt:1-2`): Qt Test, headless
  (`QCoreApplication` only), temp dirs, no network.
- Remote tests link the split set (`REMOTE_SOURCES`, `:20-28`):
  `sitequirks` + `booruengine` + 6 engine impls + `remotesource.cpp`.
- Files: `test_cachejanitor/downloadplan/explicittags/favdb/favicon/
  filehash/hydrus/hydrusstore/index/losnimport/metaparser/mediafilter/metatext/queryor/
  remotesource/scanner/security/settings/sharecatalog/shareserver/sidecarrefresh/tagsuggest/
  tagtypes/thumbloader` + `check_translations.py` (ctest case,
  `:72-78`). ViewerPage meta tail (notes bodies, comment expand layout,
  commentary preview/popup) lives in `test_metathread`.
- Fixtures: `tests/files/` (wired via `TESTFILES_DIR` for thumbloader,
  `:56-58`); `losn-sample.db` at repo root for LOSN import shapes.

## Headless GUI verification

- `Xvfb` installed: run GUI harnesses under `xvfb-run` (real graphical
  session); or `QT_QPA_PLATFORM=offscreen` for widget logic without painting.
- Mini-harness pattern: compile a small `.cpp` with
  `g++ ... $(pkg-config --cflags --libs Qt6Widgets Qt6Test)` + run `moc`
  (`/usr/lib/qt6/moc`) on any `Q_OBJECT` header first; link only the
  needed `src/...` units (mirror the `tests/CMakeLists.txt` source lists).
- `main.cpp:32-36`: message boxes are suppressed on `offscreen` (stderr
  only) - safe to run CLI paths headless.
- Single-instance lock: harnesses that touch `MainWindow`/settings must use
  a temp `XDG_CONFIG_HOME`/`XDG_DATA_HOME`, never the real `~/.config/box`.

## QA scratch

- Use `/tmp/opencode/` for throwaway harnesses, screenshots, logs, test
  `.ini`/DBs (pre-approved temp root). Name runs distinctly; clean up large
  artifacts when done.
- Never write into the library under test; never touch the real
  `~/.config/box`, `~/.local/share/box`, `~/.cache/box` from tests.
