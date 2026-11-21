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

## Test layout

- One binary per file: `add_unit_test(name sources...)` in
  `tests/CMakeLists.txt:5-16`; each `add_unit_test(test_X test_X.cpp ...)`
  = one `ctest` case (`tests/CMakeLists.txt:30-70`).
- Conventions (`tests/CMakeLists.txt:1-2`): Qt Test, headless
  (`QCoreApplication` only), temp dirs, no network.
- Remote tests link the split set (`REMOTE_SOURCES`, `:20-28`):
  `sitequirks` + `booruengine` + 6 engine impls + `remotesource.cpp`.
- Files: `test_cachejanitor/downloadplan/explicittags/favdb/favicon/
  filehash/index/losnimport/metaparser/mediafilter/metatext/queryor/
  remotesource/scanner/security/settings/sidecarrefresh/tagsuggest/
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
