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

## 2. Delete from disk leaves sidecars - TODO
- Grid/viewer delete removes media but `.json` sidecars linger
  (`src/ui/mainwindow_gridmenu.cpp:440`, `src/ui/mainwindow_viewer.cpp:220`).
  Current logic only deletes sidecars inside `downloadsDir` and preserves
  regenerable manifests - needs rework so orphaned sidecars are removed
  with the media (strays included).
- Verify: Xvfb delete of stray + downloaded rows, no `*.json` orphans.

## 3. Fullscreen exit drops maximized state - TODO
- `MainWindow::setViewerFullscreen` (`src/ui/mainwindow_viewer.cpp:126`)
  saves `windowState()` but exit path (`showMaximized`/`showNormal`) ends
  non-maximized when the app was maximized. Likely saved state already
  lost maximized bit or geometry restored clobbers it.
- Verify: Xvfb maximize -> F11 -> Esc roundtrip stays maximized.

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
