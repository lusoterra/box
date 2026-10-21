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

## 20. Favourite videos only play after download (bug report 2026-10-20)
- Symptom: a favourited video that is NOT downloaded won't play; the same
  video plays once downloaded.
- Suspects: fav rows with empty `file_url` and/or empty `ext` (favourited
  from contexts where the post lacked them, or pre-`file_url` rows):
  `ViewerPage::showRow` (`viewerpage.cpp:1485`) routes on `p.isRemote` +
  `p.fileUrl` + `isVideoExt(p.ext)` — empty ext misroutes to the image
  branch, empty fileUrl shows "No file URL" — while the downloaded-copy
  override in `rowToPost` (`favdb.cpp:503`) heals both (local path + ext
  from suffix), which is why downloading "fixes" it.
- [ ] Backfill/heal `file_url`+`ext` on fav rows (single-post refresh at
      view time or one-shot DB migration); ensure the viewer falls back to
      a refresh when a remote fav video lacks a file URL.
- [ ] Build + `ctest`.

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

## 22. LoliSnatcher feature parity check
- Mirror: `https://git.lusosterone.net/mirrors/LoliSnatcher_Droid` (use the
  mirror, not github).
- Compare this app's features against LoliSnatcher's and record parity gaps.
  Many gaps will be deliberate non-goals — mark each as adopt or wont-do
  with a one-line reason, so the list stays a decision log, not a todo pile.
- [ ] Survey LoliSnatcher features (search, filters, downloads, favourites,
      tags, viewer, settings, booru coverage); write up parity notes.
- [ ] File adopt-items as follow-up tasks; wont-do items stay documented.

## 23. Nuuru (Soybooru) engine support
- Instance: `https://soybooru.com/` (soyjak.st's proprietary custom booru;
  probably the only instance ever — name it `Nuuru (Soybooru)` in the
  Booru manager `Type` list).
- No public API docs known: RE needed (page-embedded JSON? XHR endpoints?
  `/api/...`? view-source + devtools on search pages first).
  Early probe (2026-10-20): `/api/docs` and `/api/posts` both answer HTTP 449
  to plain fetches — edge filtering, so RE needs a real browser session.
  Check first whether the UI calls same-origin XHR we can mirror (plus what
  auth/cookies it needs).
- Defenses (2026-10-20 note): aggressive PoW captcha stacked on Cloudflare —
  plan on the existing `powWall` quirk path from day one (browser-backed
  media fetch + cookie capture, like booru.allthefallen.moe), not the plain
  QNAM stack.
- Tags are meme-chaos (cf. glegle.gallery today): default the entry with
  `skipAutoClassify = true` (existing per-entry opt-out, prefs checkbox),
  so tags stay exactly as the API gives them instead of being force-fit
  into anime buckets.
  CORRECTION (2026-10-20, tag taxonomy spotted in the wild): soybooru tags
  are actually well-sorted — native categories Flag / Media / Meta /
  Series / Subvariant / Trend / Variant / Uncategorized (e.g. Variant:
  chudjak, cobson; Series: sidejaks; Meta: dailyjak). So do NOT blanket
  skip: preserve the native categories via the `customTags` mechanism
  (same as Szurubooru native categories), mapping only the obvious ones
  (Series->copyright, Variant/Subvariant->character, Uncategorized->general/
  untyped) and keeping the rest verbatim. `skipAutoClassify` stays available
  as a user fallback, but the default should keep this taxonomy.
  Display (checked 2026-10-20): right/viewer side needs no work — every
  `customTags` entry already renders as its own labeled group
  (`viewerpage.cpp:1613`). Left/sidebar side has only the five fixed
  buckets (`mainwindow.cpp:2340`): native tags fold into General there
  (and into one shared General in multibooru). A dedicated sixth sidebar
  section for native categories is optional extra work, not required.
- [ ] RE the search/post JSON shape (endpoints, paging, tags, file URLs).
- [ ] Add `BooruEngine::Nuuru` mapping behind `BooruEngineOps` + manager entry.
- [ ] Fixtures + unit tests (no network); build + `ctest` (+ docs).

## 24. HiddenBooru engine support (Tor hidden service, single instance)
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
  (space-separated)}`; tag search at `/api/tags` (Search action);
  comments at `/api/{docs,news,post}/comment`; post pages at
  `/post/<id>/view`. Full endpoint list: `lib/HB.pm` routes,
  response envelope: `docs/api-usage.md`, field docs:
  `lib/HB/Metadata/Post.pm` (all inside the tarball).
- [ ] Add `BooruEngine::Hiddenbooru` + mapping behind `BooruEngineOps`
      (rating letters map cleanly; relative file URLs resolved; tag
      namespaces per instance taxonomy); entry proxy guidance in prefs.
- [ ] Prefs Booru-manager `Type` list entry; badge default color?
- [ ] Fixtures + unit tests (no network — use tarball shapes); build +
      `ctest` (+ docs).

## 25. Cross-entry duplicate detection via md5
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
- [ ] Add md5 to `Post` + populate per engine; extend `dedupeKeyFor`;
      verify Tor/I2P twins collapse without touching distinct posts.
- [ ] Build + `ctest`.

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

## 27. File > Open history (recent folders)
- `library/history` exists but is write-only legacy (pruned in
  `showWelcome`, cleared in prefs, never appended; welcome shows last
  place instead). Put it to work, mouse-first: File > Open lists just
  the last ~5 folders inline (click reopens as a tab), then `Folder…`
  (browse) and `Open history…` (full-list picker). Record every folder
  opened via File > Open (prepend, dedupe, cap 10, persist); prune dead
  entries when the menu shows.
- [ ] Append on `openFolder`; history submenu (5 inline + Folder… +
      Open history… picker); prune missing on show.
- [ ] Build + `ctest` (+ docs).

## 28. Windowed grid (don't hold everything in memory)
- A 700-pic read-only archive fills memory: `fillModel` cards every row
  and infinite scroll only ever appends (`browseMerged` grows without
  bound; thumbs are LRU-capped at 600 but rows/pixmaps/delegate work
  are not). Same on endless online scrolling.
- Direction: window the grid model (~100-200 rows around the viewport):
  keep loading ahead on scroll-down as today, but drop rows far behind
  (stable scroll anchor required), same cap for browse appends. Watch
  out for: scrollbar jumps, text-find across the window edge, multi-
  select and viewer stepping across dropped rows.
- [ ] Windowed `fillModel`/append path with anchor-stable unload;
      verify a 700-pic archive stays flat while scrolling both ways.
- [ ] Build + `ctest`.

## 29. Ugoira downloads as animated webp/jxl (never gif)
- Treat all animated pictures (ugoira, gifs, webps, …) as GIFs for
  classification/filtering — but never convert anything TO gif (poor
  colouring, dithering).
- Download ugoira as animated webp or animated jxl instead (user choice
  in Preferences — easier on disk, loads faster). Open question: how to
  detect the framerate (frame delays live in pixiv metadata, not the
  zip — check what Danbooru exposes, else a fixed default + setting).
- [ ] Ugoira detection at download time; webp/jxl encode choice in
      prefs; framerate source decided; verify playback + thumbs.
- [ ] Build + `ctest` (+ docs).

## 30. Video audio keeps playing after leaving the viewer — DONE
##    (`backToGrid` + `showWelcome` now call `ViewerPage::stopPlayback`;
##    the player used to live on after its page hid; build OK, 25/25 tests)
- [x] Stop playback when returning to the grid or home.
- [x] Build + `ctest`.
- [x] Stop playback when returning to the grid or home.
- [x] Build + `ctest`.

## 31. Shrink the codebase (dead-code cleanup, very last)
- The codebase has grown big: audit for dead code, retired helpers,
  unused settings/strings, and machinery made obsolete by earlier tasks,
  and remove it. Behaviour must not change. Allowed lever: bump the
  language standard to C++20 where it deletes hand-rolled helpers.
- [ ] Survey for unused functions/members/strings (compiler warnings,
      `grep` for single-use sites, test-only leftovers); delete with
      justification per removal. C++20 bump where it shrinks code.
- [ ] Build + full `ctest` after each removal batch.
