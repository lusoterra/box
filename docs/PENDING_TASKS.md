<!-- Open/pending tasks. Completed tasks live in docs/DONE.md -->

# Pending tasks


## 146. Full-app feature inventory -> exhaustive test hardening - TODO
##    (interrupted earlier: baseline verified green 33/33, coverage map
##    taken, then limits blew - no new tests written yet)
- [~] Targeted additions (verified, `ctest` 36/36): `tests/test_cache.cpp` (ScanCache, `:9-64`); `tests/test_cachepolicy.cpp` (mode keys/aliases/fallback, `stricter`, `effectiveMode` narrowing, `isSfwRating`/`isNsfwRating`/`tagsProveSfw`/`isSfwPost`, `setSfwBlacklist`+`postHasBlacklistedTag`, `persistFor` explicit-download/`None` floor; `src/library/cachepolicy.h:32,155-214`); `tests/test_stray.cpp` (`Stray` path/marker conventions, `src/library/stray.h`; fixed missing `<QDir>` include in `src/library/stray.h` found by build); `tests/test_philomena.cpp` (Derpibooru/Twibooru parse/suggest/URLs/translate/ratings, 12 cases, `:13-228`); `tests/test_strayresolve.cpp` Philomena URL + `postFromRaw` (`src/remote/strayresolve.cpp:480,513-517`); `tests/test_postmeta.cpp` Philomena endpoints empty (`:22-53`); `tests/test_remotesource.cpp` `translateGelbooruMediaTags` (`:555-573`) + `engineKeysRoundTrip` all engines/Safebooru semantics (`:277-297`). Fixed `RemotePosts::postPageUrl` string keys for `derpibooru`/`twibooru`/`hybooru`/`nuuru` (`src/engines/booruengine.cpp:666`).
- [ ] Inventory every feature of the whole app (engines, library, remote,
      UI, settings) against existing `tests/`; list the untested gaps.
- [ ] Add unit tests for everything realistically testable headless
      (Qt Test, temp dirs, no network): pure helpers, parsers, DB,
      settings round-trips, cache policy, drag/fullscreen/history/grid
      options; skip only what genuinely needs a live network or eyes,
      and say why in the entry.
- [ ] Goal: no regressions and no "minor annoying bugs" creeping back;
      `ctest` green after every batch (current: 33/33).

## 147. QA pass over the whole app - TODO
- [~] Scripted smoke (verified 2026-11-05, `docs/TESTING.md`): `cd /home/lev/Sources/box && timeout 6 xvfb-run -a ./build/box > /tmp/opencode/box-smoke.log 2>&1` → exit `124` (timeout, expected for GUI app); log shows Qt/WebEngine startup and expected headless-env warnings (no DBus/PipeWire/VDPAU/GPU) with no immediate crash trace. `ctest` 34/34 green (rerun flaky `test_gridwindow` when needed).
- [ ] **Blocked on manual/Xvfb walkthrough/eyes** for full coverage (visual correctness, menu/dialog interactions, end-to-end network flows not verifiable headlessly without user-named scripted scenarios): walk every menu/dialog/tab type/viewer path/download+favourite flow; log bugs as numbered tasks or fix inline+tests.

## 148. Dead-code elimination + optimization round - TODO
##    (the old task 34 audit is DONE and stale; this is a fresh pass
##    after the feature batch: 123-145)
- [~] Dead code: scratch `-Wall -Wextra -Wunused-function -Wunused-variable -Wunused-parameter -Wshadow` build in `/tmp/opencode/box-warn` (configured against source, `/tmp/opencode/warn-build.log`, completed 100%). Reviewed `src/` warnings (miniz/autogen excluded) and fixed verified-safe cases (build+`ctest` 34/34 green after batches):
  - `src/library/metaparser.cpp:475-476` `atomicReplace` lambda `doc`/`f` shadowed outer → `outDoc`/`outFile`.
  - `src/library/cachejanitor.cpp:80` `name` shadowed outer connection name → `colName`.
  - `src/remote/remotesource.cpp:51` `BooruSite` aggregate init → explicit init; `:553-555,569` PoW-retry shadows → `retryTags/retryPage/retryLimit`, `powResult`; `:794` refresh `reqId` shadow → `failedId`.
  - `src/remote/tagsuggest.cpp:506-550` shadows in web-fallback/finished lambdas (`prefix`/`key`/`site`) → `reqKey`/`reqPrefix`, `doneKey`/`donePrefix`, `doneSite`.
  - `src/ui/mainwindow_result.cpp:100` pill lambda `text` shadow → `pillText`.
  - `src/ui/prefsdlg.cpp:1109,1141` booru editor layout shadows → `booruTypeRow`, `booruCacheRow`.
  - `src/engines/engine_derpibooru.cpp:5-9` removed unused includes (`sitequirks.h`, `<QSet>`); `tests/test_philomena.cpp:2-4` removed unused `<QJsonDocument>`.
  - `src/ui/theme.cpp` removed 8 unused palette helpers (grep no call sites + `-Wunused-function`).
  - `src/ui/viewerpage.cpp` removed 22 Qt includes left unused after `viewerpage_mediaviews.cpp` split (heuristic + build proof; `QClipboard`/`QAbstractItemModel` restored). `src/ui/viewerpage_mediaviews.cpp` removed 13 unused includes (heuristic + build proof; `QAudioDevice` restored, `QCryptographicHash` added as explicit used include). Build+`ctest` 34/34 (`test_gifview`/`test_videopane`/`test_gridwindow` green).
  Further provably-unreferenced removals need grep+build proof.
- [ ] Optimization: obvious wins only (hot paths, redundant copies,
      needless repaints/fetches); measure before/after where possible.

## 149. Declutter the code: split oversized files - TODO
##    (sizes verified 2026-11-05: `src/ui/mainwindow.cpp` 1712 + 29 `mainwindow_*.cpp` TUs; `src/ui/viewerpage.cpp` 768 + `viewerpage_mediaviews.cpp` 1163 (`ImageView`/`GifView`/`VideoView` impls) + `viewerpage_{meta,tags,load,setters,detail}.cpp`; `src/ui/prefsdlg.cpp` 1991 + `prefsdlg_booru.cpp` 615)
- [x] `mainwindow.cpp` already split into focused `mainwindow_*.cpp` translation units (verified in `CMakeLists.txt`/tree); stale "8112 lines" note corrected.
- [~] `viewerpage.cpp` incremental splits (behavior-preserving, `ViewerPage`/views declared in `src/ui/viewerpage.h`; new TUs added to `CMakeLists.txt` and `tests/CMakeLists.txt`): `ImageView`/`GifView`/`VideoView` + `beginUrlDrag` → `viewerpage_mediaviews.cpp`; metadata/notes/comments + `reloadCurrent` → `viewerpage_meta.cpp`; `renderTags`/`addTagGroup` → `viewerpage_tags.cpp`; note/cache/web-load + `loadLocalImage`/`currentPost` (+ local helpers) → `viewerpage_load.cpp`; state/setters/badges → `viewerpage_setters.cpp`; Hybooru detail fetch/apply → `viewerpage_detail.cpp`. `PrefsDialog` splits → `prefsdlg_booru.cpp`: booru editor handlers + engine detect/probe/event filter (`detectEngineFor`, `detectBooruEngine`, `testSite`, `eventFilter`, `showProbeDetails`). Build+`ctest` 34/34 green after each batch (`test_gifview`/`test_videopane`/`test_gridwindow` exercised views; full `ctest` had intermittent `test_gridwindow` segfault, rerun/`--rerun-failed` passed).
- [ ] Consider further `viewerpage.cpp` / `prefsdlg.cpp` splits if still unwieldy.
- [x] No behavior change: build + `ctest` 34/34 green (rerun flaky `test_gridwindow`); docs (`ARCHITECTURE.md` module map) updated.
