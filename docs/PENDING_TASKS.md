# Pending tasks (from user goal)



## 6. Open-externally streams remote media + player pickers (TODO)
- "Open externally" only opens media in the default player when
  downloaded/cached; otherwise it opens the URL in the browser. Remote
  posts should download somewhere usable (even with caching off - /tmp
  that survives until playback) or stream.
- Add Settings pickers: image viewer + video player (+ CLI flags/loop for
  video/gif) defaults (mpv-style). Files: `viewerpage` (Open externally),
  `settings`/`prefsdlg` (new keys).

## 7. Double-click out of fullscreen un-maximizes (TODO)
- Maximized window + double-click into immersive fullscreen, double-click
  again to exit -> window comes back non-maximized. Distinct from task 3
  (F11/Esc path). Likely mouse double-click handling in `viewerpage` /
  `MainWindow::setViewerFullscreen`. File: `src/ui/mainwindow_viewer.cpp`,
  `src/ui/viewerpage*.cpp`.

## 8. Drag needs Shift hold (TODO)
- Media drag currently anything drags; conflicts with area panning. Require
  holding Shift to start a drag (`src/ui/mainwindow.cpp`, grid drags).

## 9. Mascot opacity default 35% (TODO)
- Change default mascot opacity to 35% (`src/settings.h`,
  mascot opacity spin/sliders in `src/ui/prefsdlg.cpp`).

## 10. Booru badge auto from name (TODO)
- Booru manager badge text: auto-pick acronym from first letters of the
  box (Preferences > Browse > Booru manager, `src/ui/prefsdlg_booru.cpp`).

## 11. Soybooru autocheck fails (TODO)
- Engine autodetect falls for Soybooru (wrong detection). Fix Detect
  engine probe for soybooru.com (`detectEngineFor` in
  `src/ui/prefsdlg_booru.cpp`, `src/engines/engine_*`).

## 12. Default-tab dropdown limits (TODO)
- General > Default tab should only offer: Welcome home, Last session,
  Default booru (driven by Preferences > Browse > Default boorus).
  Remove per-booru entries (`src/ui/prefsdlg.cpp`).

## 13. Preferences/settings terminology (TODO)
- Note: user uses "preferences" and "settings" interchangeably. Docs +
  UI strings to unify (pick "Preferences").

## 1. Post URL shapes for Szuru-family hosts - DONE
- Root-id hosts (`yosho.io/31121`, `jej.lol`, `boymoders.com`,
  `bury.pink`, `flube.supply`, `spikedog.school`) -> `/<id>` in
  `postPageUrl` (`src/engines/booruengine.cpp:666`); `postPageUrlFor`
  delegates (`src/remote/remotesource.cpp:973`). `/post/` kept elsewhere
  (`szuru.libre.moe/post/6556` OK; visuabusters `/booru/post/2654` via a
  `/booru`-based URL). Detect probes `<base>` + `<base>/booru` for the Szuru
  endpoints and adopts the winning base (`src/ui/prefsdlg_booru.cpp:417`).
- Verify: `postPageUrls` fixtures (`tests/test_remotesource.cpp:914`) +
  live HEADs (yosho/libre 200, visuabusters behind Cloudflare); 37/37.

## 2. Delete from disk leaves sidecars - DONE
- Grid/viewer delete orphaned sidecars: favourited regenerable files were
  preserved (`keepSidecar`) and the lone `.json` could never re-scan.
  Now both delete paths remove both sidecar candidates with the media
  (`DownloadManager::sidecarPathsFor`, `src/remote/downloadmanager.h:55`).
- Verify: `sidecarPathsFor` in `tests/test_downloadplan.cpp`; Xvfb
  favourited-stray delete left no `.json`; 37/37.

## 3. Fullscreen exit drops maximized state - DONE
- `MainWindow::setViewerFullscreen` (`src/ui/mainwindow_viewer.cpp:126`)
  lost maximized via the showMaximized()/showNormal() pair. Now captures
  `windowState()` before chrome hides and restores it wholesale with
  `setWindowState()` on exit.
- Verify: Xvfb F11 (fills) -> Esc roundtrip back to entry size; Qt state
  threading probed offscreen (test would need a WM for the bit, so no
  unit test). 37/37.

## 4. Szuru comments fetch floods post - DONE
- `/api/comments/?postId=` is ignored by the API (whole feed returned).
  Scoping via the query language (`query=post:<id>`); limit 100
  (`src/remote/postmeta.cpp:291`). Parse unchanged.
- Verify: live `jej.lol` (`postId` -> total 265 across posts; `post:` ->
  3 for one post), `commentsUrls` fixture in `tests/test_postmeta.cpp`;
  37/37.

## 5. Unmounted config dir silently falls back - DONE
- Config override chain (`src/main.cpp:86`) joined the storage safeguard:
  missing custom dir warns (Use defaults / Change folders / Quit), name
  `Config:` in the details. Missingness is captured pre-load (opening
  the QSettings file creates the parent dir, hiding the gap). Use
  defaults drops the override; Change re-points + writes the stub and
  reloads settings.
- Verify: Xvfb `BOX_CONFIG_DIR=/nonexistent-…` showed the warning with
  `Config:` details, Use defaults reached the window; 37/37.
[rest omitted]
