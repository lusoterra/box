<!-- Open/pending tasks. Completed tasks live in docs/DONE.md -->

# Pending tasks


## 146. Full-app feature inventory -> exhaustive test hardening - TODO
##    (interrupted earlier: baseline verified green 33/33, coverage map
##    taken, then limits blew - no new tests written yet)
- [~] Targeted additions (verified, `ctest` 34/34): `tests/test_cache.cpp` (ScanCache round-trip/hit-miss + `finish` prunes unseen, temp `XDG`-isolated via `setAppCacheDirOverride`, `:9-64`); `tests/test_philomena.cpp` (Derpibooru v1/Twibooru v3 parse single+search+rating mapping incl. explicit/questionable/semi-grimdark+malformed/minimal+`parseSingle` ok=false on bad JSON, tag suggest, engine keys/labels/`shortFor`, `searchUrl`/`refreshUrl`/`postPageUrlFor`, string-key `postPageUrl`, `translateSearchTags` metatag stripping; 12 cases, `:13-215`); `tests/test_strayresolve.cpp` Philomena URL cases (`/images/<id>`, `/posts/<id>` includes Twibooru); `tests/test_postmeta.cpp` Philomena notes/comments endpoints return empty (`:22-52`, `src/remote/postmeta.cpp`); `tests/test_remotesource.cpp` `translateGelbooruMediaTags` (Gelbooru/Safebooru/Moebooru drop bare `animated`/`animated_gif`/`video`, keep `ugoira`; `src/engines/booruengine.cpp:470-490`). Fixed `RemotePosts::postPageUrl` string-key handling for `derpibooru`/`twibooru`/`hybooru`/`nuuru` (`src/engines/booruengine.cpp:666`).
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
- [ ] Manual/headless run (Xvfb per `docs/TESTING.md`): walk every menu,
      dialog, tab type, viewer path, download/favourite flow; log found
      bugs as tasks (or fix inline + tests), never in silence.

## 148. Dead-code elimination + optimization round - TODO
##    (the old task 34 audit is DONE and stale; this is a fresh pass
##    after the feature batch: 123-145)
- [~] Dead code: scratch `-Wall -Wextra -Wunused-function -Wunused-variable -Wunused-parameter -Wshadow` build in `/tmp/opencode/box-warn` (configured against source, `/tmp/opencode/warn-build.log`). Source warnings found and fixed with safe renames/clean init (build+`ctest` 33/33 after each batch):
  - `src/library/metaparser.cpp:475-476` `atomicReplace` lambda `doc`/`f` shadowed outer → `outDoc`/`outFile`.
  - `src/library/cachejanitor.cpp:80` `name` shadowed outer connection name → `colName`.
  - `src/remote/remotesource.cpp:51` missing-field-initializers for `BooruSite` aggregate (`presetBoorus`) → explicit `BooruSite` init; `:553-555,569` shadows in PoW-retry/search result handling → `retryTags/retryPage/retryLimit`, `powResult`; `:794` `reqId` shadow in refresh handler → `failedId`.
  - `src/engines/engine_derpibooru.cpp:5-9` removed provably unused includes (`sitequirks.h`, `<QSet>`).
  - `tests/test_philomena.cpp:2-4` removed unused `<QJsonDocument>` include.
  - `src/ui/theme.cpp` removed 8 provably unused palette helpers (`keepassxcDark/Light`, `genericLight`, `nord`, `catppuccin`, `dracula`, `atomOneDark`, `tokyoNight`; grep no call sites + strict-build `-Wunused-function`, `/tmp/opencode/warn-build.log`).
  Rebuild+`ctest` 34/34 green after batches. Further provably-unreferenced removals need grep+build proof.
- [ ] Optimization: obvious wins only (hot paths, redundant copies,
      needless repaints/fetches); measure before/after where possible.

## 149. Declutter the code: split oversized files - TODO
##    (sizes verified 2026-11-05: `src/ui/mainwindow.cpp` 1712 + 29 `mainwindow_*.cpp` TUs; `src/ui/viewerpage.cpp` 1874 + `viewerpage_{meta,tags,load,setters,detail}.cpp`; `src/ui/prefsdlg.cpp` 1991 + `prefsdlg_booru.cpp` 615)
- [x] `mainwindow.cpp` already split into focused `mainwindow_*.cpp` translation units (verified in `CMakeLists.txt`/tree); stale "8112 lines" note corrected.
- [~] `viewerpage.cpp` incremental splits (behavior-preserving, `ViewerPage` declared in `src/ui/viewerpage.h`; new TUs added to `CMakeLists.txt` and `tests/CMakeLists.txt`): metadata/notes/comments + `reloadCurrent` → `viewerpage_meta.cpp`; `renderTags`/`addTagGroup` → `viewerpage_tags.cpp`; note/cache/web-load + `loadLocalImage`/`currentPost` (+ local helpers) → `viewerpage_load.cpp`; state/setters/badges → `viewerpage_setters.cpp`; Hybooru detail fetch/apply → `viewerpage_detail.cpp`. `PrefsDialog` splits → `prefsdlg_booru.cpp`: booru editor handlers + engine detect/probe/event filter (`detectEngineFor`, `detectBooruEngine`, `testSite`, `eventFilter` probe click, `showProbeDetails`; wired in `CMakeLists.txt`/`tests/CMakeLists.txt`). Build+`ctest` 34/34 green after each batch.
- [ ] Consider further `viewerpage.cpp` / `prefsdlg.cpp` splits if still unwieldy.
- [x] No behavior change: build + `ctest` 34/34 green; docs (`ARCHITECTURE.md` module map) updated.
