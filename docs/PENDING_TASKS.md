# Pending tasks (from user goal)



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

## 4. Szuru comments fetch floods post - TODO
- `PostMeta::commentsUrl` Szuru branch uses `/api/comments/?postId=`
  (`src/remote/postmeta.cpp:291`); viewer shows the whole feed instead of
  the one post. Params/response shape likely wrong (ignores the filter).
- Verify: live Szuru post, `test_postmeta` fixtures scoped per-post.

## 5. Unmounted config dir silently falls back - TODO
- Config override chain (env `BOX_CONFIG_DIR` + `[app] configDir` stub,
  `src/main.cpp:86`) is missing from the startup safeguard that checks
  only `dataDir`/`cacheDir`/`downloadDir` (`src/main.cpp:155`). A missing
  config dir (USB unplugged) loads defaults without asking; must warn
  (Use defaults / Change folders / Quit) like the others, and never
  create files at the missing path. Note: stub pointer itself is fine.
- Verify: Xvfb with `BOX_CONFIG_DIR` pointed at a missing dir.
[rest omitted]
