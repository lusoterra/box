<!-- TEMPORARY SCRATCH FILE — delete when all tasks land. Canonical copy;
     /tmp/opencode/pending_tasks.md is a stale duplicate. -->

# Pending multi-task plan (written 2026-10-20, before starting)
# Work through step by step; tick boxes as each lands + tests pass.

## 1. Selected tab colours more visible — DONE (accent underline + bold
##    selected-tab text in `MarqueeTabBar::paintEvent`; build OK, 22/22 tests)
- [x] Find selected-tab painting (`src/ui/tabbar.cpp` MarqueeTabBar + `src/ui/theme.cpp`).
- [x] Strengthen selected state (brighter bg / accent underline / bolder text).
- [x] Build, screenshot-check under xvfb if feasible, `ctest`.

## 2. Comprehensible stdout logging + rotating log file — DONE (new
##    `src/applog.*`: level-prefixed stderr lines, spam denylist, 128MB
##    rotating box.log in data dir; verified by throwaway harness PASS)
- [x] Find current logging (`src/main.cpp`: message handler? none yet?).
- [x] Install `qInstallMessageHandler`: filter Qt/ffmpeg spam (context/category
      denylist), prefix `ERROR/WARN/INFO`, keep concise.
- [x] Log file in chosen dir (data dir? `~/.local/share/box/`): default cap
      128 MB with rotation (e.g. N x rotated files, size-checked on write).
- [x] Setting for dir/cap optional; keep defaults sane. `ctest`.

## 3. Default badge colours (user-changeable) — DONE
##    (`RemotePosts::defaultBadgeColor` in `sitequirks`; stamped on presets +
##    fresh customs; `badgeColorDefaults` test PASS; 22/22 ctest)
- gelbooru.com -> #0870fa, rule34.xxx + clones -> #55aa00,
  aibooru.online -> #44cc00, danbooru + safebooru -> #daac82,
  shimmie instances -> #7f7f7f.
- [x] Find badge color plumbing (`BooruSite::badgeColor`, presets in
      `src/remote/remotesource.cpp`, prefs booru manager editor).
- [x] Set per-host defaults for presets + newly added customs (by engine/host
      mapping); existing user values must NOT be clobbered.
- [x] Build + `ctest` (translations check for any new strings).

## 4. Multiple favourites tabs — DONE (`openFavTab()` always opens fresh;
##    `restoreTabs()` restores every saved fav tab; build OK, 22/22 tests)
- [x] `openFavTab()` currently reuses the single fav tab; allow many
      (like remote tabs), each with own tags/query/rating/sort.
- [x] Check `saveTabs`/`restoreTabs`/`lastPlace`/tab-title/`refreshFavTabs`
      paths that assume a single `isFavourites` tab; update all.
- [x] Update `docs/UI-TOUR.md` favourites section. Build + `ctest`.

## 5. Videos flashing red "can't play" while still loading — DONE
##    (VideoPane defers red label 650ms + only terminal statuses; transient
##    Loading/Buffering/Stalled errors ignored; build OK, 22/22 tests)
- [x] Find error label in `src/ui/viewerpage.cpp` (VideoView/VideoPane).
- [x] Only show the error for terminal states (InvalidMedia / EndOfMedia with
      error), not during Loading/Buffering/Stalled; or small delay.
- [x] Headless/xvfb verify if possible. Build + `ctest`.

## 6. Robust tab saving (crashes lose tabs) — DONE (synchronous
##    `saveTabsNow()` on add/close/move; atomic tmp+rename `saveSettings`
##    with `settings.ini.bak` rotation + heal-on-load; switches stay
##    coalesced; 2 new settings tests; build OK, 22/22 tests)
- [x] `stageTabs()` coalesces ini writes (1500 ms) — a crash inside the window
      loses everything. Save synchronously on tab add/close/move; atomic
      write (QSaveFile or tmp+rename); keep a backup of last-good tabs.
- [x] Build + `ctest`.

## 7. Colour the left-hand tag menu — DONE (`rebuildTagTab` paints items
##    with `UiIcons::tagBucketColor(bucket)`; new `test_tagcolors` palette
##    test; build OK, 23/23 tests)
- [x] Sidebar tag lists should carry bucket colours (like filter pills).
- [x] Build + `ctest`.

## 8. Slow/unloading videos wedge the app, then segfault — DONE
##    (software FFmpeg decoding forced via `QT_FFMPEG_DECODING_HW_DEVICE_TYPES=,`
##    in `main.cpp`; `setFrame` skips non-readable maps; thumb grabber stays
##    single-flight + 8s timeout; invalid-frame test; build OK, 23/23 tests)
- Crash log: `[AVHWFramesContext] Failed to sync surface`, `Error
  transferring the data to system memory: -5`, `Cannot map a video frame
  in ReadOnly mode!` -> SIGSEGV when opening such a video.
- [x] Force software video decoding (HW/VAAPI path is what segfaults);
      research the Qt6-ffmpeg knob, set it in-app.
- [x] Also: cap concurrent decode cost, don't let one bad video wedge UI.
- [x] Build + `ctest` (+ smoke harness if feasible).

## 9. Close fast on X / Alt+Q (KDE) — DONE (`closeEvent` hides + blanks
##    first, cancels refresher/sources/thumb pools/video, persists last;
##    `ThumbLoader::cancel` + `VideoThumbGrabber::cancel` +
##    `ViewerPage::stopPlayback` new; build OK, 23/23 tests)
- [x] `closeEvent`: hide/blank UI immediately (home page or empty stage),
      cancel scans/downloads/workers fast, then quit; never block the event.
- [x] Build + `ctest`.

## 10. Captcha/PoW wall UX — DONE (new `RemoteSource::lastWall()` flag on
##    both challenge branches; `showWallMenu` on fresh searches: per-site
##    solve-in-browser (cookies saved + re-search) or drop booru (deselect
##    in multibooru, close solo tab); `wallDetectors` test; docs; 23/23)
- [x] When a booru fails with a captcha/PoW wall, show a small menu: open the
      web view now to solve it, or close the tab / deselect that booru in
      multibooru mode.
- [x] Build + `ctest` (+ docs).

## 11. Default booru tags as cards — DONE (entry `defaultTags` render as
##    pills via `RemotePosts::defaultTagsFor` + tab `droppedDefaults`; × drops
##    the token and re-searches; helper unit-tested; docs; build OK, 23/23)
- [x] Entry `defaultTags` should render as removable cards like typed tags.
- [x] Build + `ctest` (+ docs).

## 12. Tag suggestion relevance — DONE (tiered rank: exact, prefix by
##    count, substring last; `TagSuggester::rank` for remote, same rule in
##    `Index::suggest`; stale-row prune in `onRemoteSuggest`; fetch width
##    12→24; rank/suggest tests; build OK, 23/23 tests)
- [x] Rank exact/prefix matches first; longer relevant tags beat short noise;
      dedupe irrelevant entries. Tags the user wants rarely surface today.
- [x] Build + `ctest`.

## 13. Favourite tags system — DONE (sidebar + viewer right-click star;
##    omnibar ★ menu; starred tags outrank plain prefix matches in both
##    suggest paths; `fav/tags` in settings.ini; 3 new tests; docs;
##    build OK, 23/23 tests)
- [x] Right-click a tag (sidebar or viewer) -> add to favourite tags;
      favourites get priority in search suggestions. Add a menu for them.
- [x] Persist in settings.ini; build + `ctest` (+ docs).

## 14. One-click AI-content filter — DONE (omnibar AI toggle backed by
##    `filter/hideAi`; server `-ai_*` exclusions + `isAiPost` client nets in
##    browse/local/fav paths; `aiContentTags()` shared list; settings tests;
##    docs; build OK, 23/23 tests)
- [x] Convenient button to exclude AI tags (`ai_generated`, etc.).
- [x] Build + `ctest` (+ docs).

## 15. HyBooru engine support — DONE (RE'd live instance + client bundle:
##    `/files/f{sha}{ext}`, thumbs, `/posts/<id>`, tag namespaces, star
##    ratings; `BooruEngine::Hybooru` + `engine_hybooru.cpp` with summary/
##    detail mappers; tagsuggest + refresh + viewer lazy-detail wired;
##    prefs Type + bucket grid; `test_hybooru` fixtures; docs; 24/24 tests)
- Decision: no per-host badge color (no instance branding known; the
  default stands and stays user-changeable).
- [x] Add `BooruEngine::Hybooru` + request/response mapping (search URL,
      parse, single-post refresh, sort/tag translation) behind `BooruEngineOps`.
- [x] Prefs Booru-manager `Type` list entry; badge default color?
- [x] Fixtures + unit tests (no network); build + `ctest` (+ docs).

## 16. Stale favourite/downloaded marks + ghost cards (bug report 2026-10-20) — DONE
##    (shared `favKeysByPath`/`favKeysFor` path-reverse-map in bulk fav/unfav,
##    `allFav`, single toggle + delete dedup; `remove()` reports
##    `numRowsAffected()>0`; `repaintFavMarks()` recomputes ♥/↓ + viewer
##    badges in place; `fillModel` marks via `isFavMarked`;
##    `removeMissingReportsFalse` test; build OK, 24/24 tests)
- [x] Resolve local grid posts to DB rows by path in grid fav actions;
      repaint current-tab marks + viewer badges after bulk ops; verify dead
      rows drop from fav tabs without manual Refresh.
- [x] Build + `ctest`.

## 17. Annotations (translated text overlays) — DONE (overlay boxes already
##    scaled with the image; added toolbar Notes show/hide toggle with
##    persistent per-image note store; fixed stale boxes lingering across
##    navigation; Safebooru notes/comments now use dapi (were Moebooru
##    shape); HyBooru has no separate endpoint (notes ride detail objects);
##    `test_postmeta` fixtures; docs; build OK, 25/25 tests)
- [x] Check what `postmeta` notes fetch already provides; overlay boxes with
      show/hide toggle in `ViewerPage`.
- [x] Build + `ctest` (+ docs).

## 18. Read-only comments on post view — DONE (fetch + section already
##    existed; made long threads collapsed: first 5 + show-all toggle with
##    deferred tail rebuild; `parseCommentsShapes` fixtures; docs;
##    build OK, 25/25 tests)
- [x] Check what `postmeta` comments fetch already provides; render a
      comments section in `ViewerPage` (collapsed/long-list friendly).
- [x] Build + `ctest` (+ docs).

## 19. Reverse image search buttons — DONE (shared
##    `RemotePosts::reverseSearchUrls` helper gated on http(s) file URLs;
##    viewer `Image search` toolbar menu button + grid card submenu (per-post
##    labels for multi-select); URL tests; docs; build OK, 25/25 tests)
- [x] Viewer menu/buttons + grid context actions; build + `ctest` (+ docs).

## 20. Favourite videos only play after download (bug report 2026-10-20) — DONE
##    (in-memory `ext` heal in `rowToPost` from file URL/path, no migration;
##    viewer single-post refresh fallback for empty file URLs, once per
##    post per session; `extHealedFromFileUrl` test; 25/25 tests)
- Symptom: a favourited video that is NOT downloaded won't play; the same
  video plays once downloaded.
- Suspects: fav rows with empty `file_url` and/or empty `ext` (favourited
  from contexts where the post lacked them, or pre-`file_url` rows):
  `ViewerPage::showRow` (`viewerpage.cpp:1485`) routes on `p.isRemote` +
  `p.fileUrl` + `isVideoExt(p.ext)` — empty ext misroutes to the image
  branch, empty fileUrl shows "No file URL" — while the downloaded-copy
  override in `rowToPost` (`favdb.cpp:503`) heals both (local path + ext
  from suffix), which is why downloading "fixes" it.
- [x] Backfill/heal `file_url`+`ext` on fav rows (single-post refresh at
      view time or one-shot DB migration); ensure the viewer falls back to
      a refresh when a remote fav video lacks a file URL.
- [x] Build + `ctest`.

## 21. Dropdown filters render as typed-tag pills — DONE (rating box writes
##    strip typed `rating:` tokens, typed `rating:` clears the box via
##    `normalizeTypedRating()`; pills render as `rating:<v>`/plain; docs;
##    build OK, 23/23 tests)
- Rating/category dropdowns are shortcuts for typing: a rating choice
  renders as a `rating:<value>` pill (rose metatag color), a source choice
  as a plain pill — same look and × behavior as typed tags. Typing a
  `rating:` tag clears the box to `All`; picking from the box drops
  conflicting typed `rating:`/`safety:` tags, so the two never stack.
- [x] `onRatingChanged` strips typed rating tokens; `normalizeTypedRating()`
      in `commitSearch`/`commitFilter`; pill restyles in `updateResultLabel`.
- [x] Build + `ctest` (+ docs).

## 22. LoliSnatcher feature parity check — DONE (mirror surveyed;
##    writeup in `docs/LOSNSNATCHER-PARITY.md` with ADOPT/WONT-DO/PICK;
##    adopt-items filed as tasks 31–32; engines left for user picks)
- Mirror: `https://git.lusosterone.net/mirrors/LoliSnatcher_Droid` (use the
  mirror, not github).
- Compare this app's features against LoliSnatcher's and record parity gaps.
  Many gaps will be deliberate non-goals — mark each as adopt or wont-do
  with a one-line reason, so the list stays a decision log, not a todo pile.
- [x] Survey LoliSnatcher features (search, filters, downloads, favourites,
      tags, viewer, settings, booru coverage); write up parity notes.
- [x] File adopt-items as follow-up tasks; wont-do items stay documented.

## 23. Nuuru (Soybooru) engine support — DONE (RE'd live HTML/JS + SSR
##    embeds: `/api/booru/posts` search (1-indexed), single, tags/search,
##    comments; `/api/booru/posts/<id>/file|/thumbnail`; `/post/view/<id>`;
##    native taxonomy in `customTags` with Series→copyright,
##    Variant/Subvariant→character, rest→general; `powWall` quirk;
##    prefs Type + bucket grid; `test_nuuru` real-shape fixtures; docs;
##    26/26 tests)
- [x] RE the search/post JSON shape (endpoints, paging, tags, file URLs).
- [x] Add `BooruEngine::Nuuru` mapping behind `BooruEngineOps` + manager entry.
- [x] Fixtures + unit tests (no network); build + `ctest` (+ docs).

## 24. HiddenBooru engine support (Tor hidden service, single instance) — DONE
##    (shapes from the v0.8 tarball: array search with terms/page/limit/order,
##    `id:` refresh narrowing, tag/complete suggestions, letter ratings,
##    relative URLs resolved at base; powWall quirk on both hosts; prefs
##    Type + bucket grid; `test_hiddenbooru` fixtures; docs; 27/27 tests)
- Instance: anime booru, Tor-first (I2P is extremely slow, use only as
  fallback): Tor `http://hbooru5cef5tkb7a7m5de5kjxqg6abfh3447sxw36n5pe7utnnei25yd.onion/`,
  I2P `https://hiddenbooru.i2p` (I2P socks5 `localhost:4447`, or http `4444`).
  Route via the entry proxy (`socks5h://` Tor); like Nuuru, probably the
  only instance ever — name it `HiddenBooru` in the Booru manager list.
- Source tarball (`hiddenbooru_v0.8.tar.gz`, v0.8, Perl/Mojolicious,
  gitignored) RE'd 2026-10-21: `GET /api/posts?terms=&page=` (1-indexed,
  matches box) `&limit&order=ASC|DESC` -> array of `{type:"post", id,
  hash, htype (sha1/md5), mtype ('i'/'v'), fmt, size, width, height,
  rating ('u'/'s'/'q'/'e' letter or full name), file_url (may be
  RELATIVE — resolve against base), preview_url, source_url, tags
  (space-separated)}`; tag search at `/api/tag/complete?term=` (flat
  string array); comments are create/delete only (no list endpoint);
  post pages at `/post/<id>/view`. Full endpoint list: `lib/HB.pm` routes,
  response envelope: `docs/api-usage.md`, field docs:
  `lib/HB/Metadata/Post.pm` (all inside the tarball).
- [x] Add `BooruEngine::Hiddenbooru` + mapping behind `BooruEngineOps`
      (rating letters map cleanly; relative file URLs resolved; tag
      namespaces per instance taxonomy); entry proxy guidance in prefs.
- [x] Prefs Booru-manager `Type` list entry; badge default color?
- [x] Fixtures + unit tests (no network — use tarball shapes); build +
      `ctest` (+ docs).

## 25. Cross-entry duplicate detection via md5 — DONE (`md5` on `Post`,
##    populated in `finishRemote` from `md5`/`checksumMD5` (validated hex);
##    `dedupeKeyFor` keys remote rows site-agnostically when present;
##    `md5DedupeKey`+`isMd5Hex` unit-tested, HyBooru propagation tested;
##    fav rows stay site+id (no DB column); build OK, 27/27 tests)
- Same booru reachable at two URLs (Tor onion + I2P HiddenBooru, or any
  mirror pair) cards every post twice in multibooru: `dedupeKeyFor` keys
  remote rows by site+id, so cross-entry twins never collapse. No
  perceptual/AI hashing — plain md5 equality is enough (plus eyes).
- Direction: carry `md5` on `Post` (populated from APIs that provide it:
  danbooru `md5`, HyBooru `md5`, HiddenBooru `hash`+`htype`, else
  content-hash on download like the rename flow), then key remote dedupe
  by md5 when present (site-agnostic), falling back to site+id.
  Twins collapse in grid (`fillModel`) and viewer stepping like
  local/remote twins do today.
- [x] Add md5 to `Post` + populate per engine; extend `dedupeKeyFor`;
      verify Tor/I2P twins collapse without touching distinct posts.
- [x] Build + `ctest`.

## 26. Media-type filter dropdown (stills / GIF / animated / video / sound) — DONE
##    (dropdown writes single `filetype:` tag pills; client matching in
##    `matchesTokens`/`index.query`/browse net; Danbooru-only passthrough;
##    combos `AdjustToContents`; filetype tests; docs; 25/25 tests)
- Quick dropdown next to the omnibar: still images, animated GIFs
  (ugoira + animated webp/jxl all count as GIF — no separate ugoira
  kind), anything animated, videos, sound (implies video: audio-carrying
  formats). Like the other dropdowns it just adds a typed tag pill
  (`filetype:` single-select; picking replaces existing `filetype:` tags,
  typing one clears the box).
- Client matching is ext-based (`Post::ext` vs the token's comma list;
  mirrors the grid's own video/GIF chips, so ugoira-served-as-video stays
  video). Server passes `filetype:` through on Danbooru only (verified
  support); stripped elsewhere with the client net covering. Buttons that
  elide their text get `AdjustToContents` widths.
- [x] Media dropdown + `filetype:` client matching (`matchesTokens`,
      `index.query`, browse net); per-engine pass/strip; pill + tests.
- [x] Build + `ctest` (+ docs).

## 27. File > Open history (recent folders) — DONE (last folders inline
##    in the Open menu + `Folder…` + full-list picker; `noteFolderOpened`
##    helper records opens, prunes dead on show; unit test; docs;
##    build OK, 27/27 tests)
- `library/history` exists but is write-only legacy (pruned in
  `showWelcome`, cleared in prefs, never appended; welcome shows last
  place instead). Put it to work, mouse-first: File > Open lists just
  the last ~5 folders inline (click reopens as a tab), then `Folder…`
  (browse) and `Open history…` (full-list picker). Record every folder
  opened via File > Open (prepend, dedupe, cap 10, persist); prune dead
  entries when the menu shows.
- [x] Append on `openFolder`; history submenu (5 inline + Folder… +
      Open history… picker); prune missing on show.
- [x] Build + `ctest` (+ docs).

## 28. Windowed grid (don't hold everything in memory) — DONE (pixmaps

## 29. LoliSnatcher "merge" orphans: restore parent booru + no-redownload reimports — DONE
##    (file-URL host fallback + donmai CDN rule; `md5sOnDisk` cross-folder
##    skip on reimport; merge-row + md5 tests; 29/29 tests, build OK)
- Symptom: items from LoliSnatcher archives/DB imports are named `merge_*`
  and lose their parent booru (merge-tab downloads carry no per-post
  postUrl, so `siteForHost` in `losnimport.cpp:215` cannot resolve them;
  `targetPaths` then files them under a literal `merge` host folder).
  Local `merge_*` sidecars DO still carry file URLs (and tags/post links)
  pointing at real boorus — use those.
- Fix direction: (a) resolve each merge row's booru from its file URL
  host (CDN→site map like `fsTagForPost` uses in reverse) and/or post
  tags, file imports under the proper host folder with a proper
  `<host>_<md5>` stem (+ sidecar); scanned `merge_*` archives heal the
  same way via their sidecars; (b) reimporting the same DB must skip
  what's already on disk by md5 (verify the md5-glob skip in the import
  path covers cross-folder hits, like `relinkFavourites` does).
- [x] Per-row booru resolution for merge imports + host-folder filing;
      md5-based skip on reimport; verify with a merge-named fixture.
- [x] Build + `ctest`.

##    windowed, not rows; geometry-based visible range; LRU cap 600→256;
##    `test_gridwindow` verifies flat memory on a real 700-file archive;
##    build OK, 28/28 tests)
- A 700-pic read-only archive fills memory: `fillModel` cards every row
  and infinite scroll only ever appends (`browseMerged` grows without
  bound; thumbs are LRU-capped at 600 but rows/pixmaps/delegate work
  are not). Same on endless online scrolling.
- Direction: window the grid model (~100-200 rows around the viewport):
  keep loading ahead on scroll-down as today, but drop rows far behind
  (stable scroll anchor required), same cap for browse appends. Watch
  out for: scrollbar jumps, text-find across the window edge, multi-
  select and viewer stepping across dropped rows.
- [x] Windowed `fillModel`/append path with anchor-stable unload;
      verify a 700-pic archive stays flat while scrolling both ways.
- [x] Build + `ctest`.

## 30. Ugoira downloads as animated webp/jxl (never gif) — DONE
##    (vendored miniz; download-time convert via ffmpeg/cjxl with pixiv
##    frame delays + configured fallback; `webp`/`jxl`/`zip` + delay prefs;
##    `test_ugoira` incl. real encode; found: Danbooru serves .zip + optional
##    pixiv_ugoira_frame_data, Safebooru pre-converts to .gif; build OK, 29/29 tests)
- Treat all animated pictures (ugoira, gifs, webps, …) as GIFs for
  classification/filtering — but never convert anything TO gif (poor
  colouring, dithering).
- Download ugoira as animated webp or animated jxl instead (user choice
  in Preferences — easier on disk, loads faster). Framerate: pixiv
  frame delays when present, else the configured default.
- [x] Ugoira detection at download time; webp/jxl encode choice in
      prefs; framerate source decided; verify playback + thumbs.
- [x] Build + `ctest` (+ docs).

## 31. Video audio keeps playing after leaving the viewer — DONE
##    (`backToGrid` + `showWelcome` now call `ViewerPage::stopPlayback`;
##    the player used to live on after its page hid; build OK, 25/25 tests)
- [x] Stop playback when returning to the grid or home.
- [x] Build + `ctest`.

## 32. Settings backup/restore (JSON export/import) — DONE (portable
##    `settingsexport` module: settings + boorus + favs; File menu
##    Backup/Restore; boorus merge by key, favourites dedupe on import;
##    roundtrip/merge/reject tests; docs; build OK, 30/30 tests)
- Offline answer to LoliSync-as-backup (parity task 22): export boorus,
  settings, fav tags and favourites to one JSON file; import merges it
  back (tabs optional). No server, no account.
- [x] Export/import round-trip ( prefs dialog or File menu); fixtures;
      build + `ctest` (+ docs).

## 33. Video playback speed control — DONE (rate menu button in the
##    video controls, 0.25×–2× via `setPlaybackRate`, session-sticky
##    across videos; crash-safety test; docs; build OK, 30/30 tests)
- Viewer video menu: playback rate (0.25×–2×) via `QMediaPlayer::setPlaybackRate`.
  Tiny, complements the Image-search menu.
- [x] Rate menu + persist per session; build + `ctest` (+ docs).

## 34. Shrink the codebase (dead-code cleanup, very last) — DONE
##    (`-Wall -Wextra` audit in a scratch build dir: removed unused
##    `hiddenbooruIsVideo`, dead `downloadable` local, fixed member
##    init-order warnings, default-initialized `BooruSite::engine`
##    (was indeterminate), added missing Nuuru/Hiddenbooru post-page
##    cases; refreshed stale ARCHITECTURE line refs. C++20 evaluated:
##    no shrinkage without churn (Qt containers already have
##    `contains()`), so the codebase stays on C++17; build OK, 30/30 tests)
- The codebase has grown big: audit for dead code, retired helpers,
  unused settings/strings, and machinery made obsolete by earlier tasks,
  and remove it. Behaviour must not change. Allowed lever: bump the
  language standard to C++20 where it deletes hand-rolled helpers.
- [x] Survey for unused functions/members/strings (compiler warnings,
      `grep` for single-use sites, test-only leftovers); delete with
      justification per removal. C++20 bump where it shrinks code.
- [x] Build + full `ctest` after each removal batch.

## 35. Smarter auto favicon fetching (parse page HTML for icon links) — DONE (`faviconIconsFromHtml` + tried-URL guard; fixture tests; 30/30)
- `FaviconCache` only tries `<base>/favicon.ico` (+ host root): sites
  without one keep the generic badge even when their HTML advertises an
  icon via `<link rel="icon"|"shortcut icon"|"apple-touch-icon" ...>`.
- Direction: when a candidate fetch returns HTML instead of an image,
  parse `faviconIconsFromHtml()` (pure, unit-tested) for link-rel icon
  hrefs, resolve relative against the page URL, and try those before
  marking the key dead. Keep the .ico-first order (cheap hit stays fast).
- [x] HTML icon-link parsing + candidate-chain fallback; fixture tests.
- [x] Build + `ctest` (+ docs).

## 36. Webview auto-pops on captcha/Cloudflare walls (opt-out) — DONE (`wallAutoPopup` default-on + prefs checkbox; auto-solve-first-wall; round-trip tests; 30/30)
- Today a wall means a status-bar line + a menu you must know to look
  for (`showWallMenu` on fresh searches only); the solve browser never
  opens itself. It should pop open on the walled site by default, with
  a Preferences toggle to turn the auto-popup off (menu behaviour stays).
- Direction: `wallAutoPopup` setting (default on, persisted + backed up);
  checkbox in Preferences; `onRemoteFinished` opens `solveWallInBrowser`
  for the first wall site when set, menu otherwise. No popups on
  infinite-scroll appends (same gate as the menu).
- [x] Setting + prefs checkbox; auto-solve-first-wall behaviour; test.
- [x] Build + `ctest` (+ docs).

## 37. Status-bar solve button next to failed-query info — DONE (permanent right-end `Solve … in browser…` button; menu when several; 30/30)
- A failed query leaves only a timed status-bar line (click = probe
  dialog with raw request/response). Next to that info there should be
  a one-click button opening the webview to grab cookies / ride PoW /
  solve Cloudflare for the walled site(s).
- Direction: `m_wallSolveBtn` on the status bar, visible while the last
  search reported wall sites; click opens the solve browser (menu when
  several). Qt temporary messages cover left-docked widgets, so it
  docks as a permanent (right-end) status-bar widget to stay clickable
  while the message shows. Hides on new search / clean result.
- [x] Button + visibility wiring; build + `ctest` (+ docs).

## 38. Soybooru 449 walls: detect pow/captcha + guided re-capture
- Live RE (2026-10-21): API answers HTTP 449 `{"error":"pow_required"}`
  (and `captcha_required` for the hCaptcha variant); homepage returns 200
  with no cookies and fires no API calls. PoW = WASM worker
  (`/api/pow/challenge` -> `/api/pow/verify`, auto-solves in-page);
  captcha = hCaptcha (`/api/captcha/*`, human). Both only trigger on
  bundle-driven pages, so capture-at-homepage mints nothing — the loop
  the user hit. App also never recognized 449 (no wall flag/menu/button,
  cryptic "Unexpected response shape").
- Direction: treat 449 + either token as a wall (no hidden-browser retry
  for bare JSON — no JS runs there); thread the wall kind into the
  capture dialog with tailored guidance (search in-page so the challenge
  appears, then save); wallDetectors tests + docs.
- [ ] 449 branch + token detection; kind-aware capture guidance.
- [ ] Build + `ctest` (+ docs).
