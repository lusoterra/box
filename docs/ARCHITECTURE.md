# ARCHITECTURE - box (booru client, Qt6 C++)

Native C++/Qt6 client for local image archives + online boorus. Local
archives are read-only; downloads + favourites feed back into offline browsing.
Entry: `src/main.cpp:40` (`box`, single-instance via `session.lock`).

- Process/thread model: one GUI-thread process (second copy exits,
  `src/main.cpp:112`). Close is instant: `closeEvent` (`mainwindow.cpp`)
  hides + blanks the stage first, cancels scans/downloads/fetchers/thumb
  work, then persists - teardown waits never hold the event open.
  - `Scanner::scan` is blocking, run via `QtConcurrent::run`
    (`src/library/scanner.h:26`); progress via signals on owner.
  - `ThumbLoader` owns a `QThreadPool` (`src/remote/thumbloader.cpp:26`);
    each worker thread keeps one persistent QNAM (connection reuse for
    the whole grid), and per-card `thumbnailQueued/Started/Progress/
    Failed` signals drive the grid's muted spinner/progress veil.
    viewer decode via `QtConcurrent::run` (`src/ui/viewerpage.cpp:996`);
    tag-suggest web fetch via `QtConcurrent::run`
    (`src/remote/tagsuggest.cpp:311`); prefs probe/test via
    `QtConcurrent::run` + `QFutureWatcher` (`src/ui/prefsdlg.cpp:1315,1358`).
  - Network is async QNAM: `RemoteSource::searchFinished`,
    `DownloadManager` (`kMaxFlight = 3`, `src/remote/downloadmanager.h:109`),
    `TagTypeFetcher` (`kMaxFlight = 4`, `src/library/tagtypes.h:171`),
    `SidecarBatch` (`kFlights = 3`, `src/library/sidecarrefresh.h:53`).
  - SQLite connections are per-thread, never shared (`src/library/cache.h:14`,
    `src/library/tagtypes.h:82`).

## Module map

- `src/engines/` - per-engine API shapes behind `BooruEngineOps`
  (`src/engines/booruengine.h:99`: `searchUrl`/`parseSearch`/`refreshUrl`/
  `parseSingle`/`applyAuth`; `engineOps()` at `:146`).
  - Pure shared helpers in `RemotePosts`: `translateSearchTags`,
    `sortSearchTag`, `isTransientSearchError`, `fromDanbooru/Gelbooru/Moebooru/
    Szuru/Shimmie/Hybooru/Nuuru/E621/AnimePictures`, `fromPhilomenaFamily`,
    `parseShimmieXml` (`src/engines/booruengine.h:25-95`). `engine_philomena.cpp`: the
    Philomena family, one implementation with two `PhilomenaShape` rows -
    Philomena (Elixir; Derpibooru v1 `/api/v1/json/search/images|images/:id`,
    `{images}`/`{image}`) and Booru-on-Rails (Ruby; Twibooru v3
    `/api/v3/search/posts|posts/:id`, `{posts}`/`{post}`). Shared fields:
    `representations`, ratings from `safe`/`suggestive`/`questionable`/
    `explicit` tags, namespaced `artist:`/`character:`/`oc:` buckets,
    Markdown `description`. Helpers: `isPhilomenaFamily`,
    `philomenaApiRoot/ListKey/ItemKey/ItemPath`. Engine keys are
    `philomena`/`booru-on-rails`; legacy `derpibooru`/`twibooru` still load.
    Nuuru notes: plain fetches hit HTTP 449 `{"error":"pow_required"}`
    (or `captcha_required`: hCaptcha via `/api/captcha/*`); PoW is a WASM
    worker (`/api/pow/challenge` → `/api/pow/verify`). The token embeds
    the solving egress IP, so the app solves PoW natively on the searching
    QNAM (`PowSolver`: same proxy/egress/jar, one retry, tokens merged back
    into the entry) instead of relying on browser-harvested cookies that die
    on any IP change. Only captchas still need the human browser path.
    Traffic rides the browser-backed stack (`powWall` quirk on
    `soybooru.com`). Cloudflare Turnstile instances (`browserApi` quirk on
    `danbooru.donmai.us`/`safebooru.donmai.us`/`cdn.donmai.us`) never let a
    harvested `cf_clearance` authorize QNAM, so searches, refreshes, tag
    suggestions, media and thumbnails go through the capture profile. If that browser fallback passes Cloudflare but the
    origin still answers `pow_required`, `RemoteSource::beginPowRetry`
    solves natively and retries the search automatically
    (`remotesource.cpp:879`);
    ATF notes: the wall is an `atf-anti-bot` cookie (minted by the checkbox
    verification page; rotates, so re-capture is routine) plus per-endpoint
    captchas. Media/file hosts challenge independently of `/`, so capture
    and re-solve target the failing URL (`PendingSite::failUrl`,
    `DownloadSummary::wallTargets`), never just the homepage.
    files at `/api/booru/posts/<id>/file`, thumbs at `…/thumbnail`, native
    Flag/Media/Meta/Series/Subvariant/Trend/Variant/Uncategorized taxonomy
    in `customTags`.
    HyBooru notes: summaries are tagless (viewer lazy-fetches detail on
    open); `rating` is a star score (all posts read `general`, server
    `rating:`/`sort:` tokens stripped); files at `/files/f{sha}{ext}`,
    thumbs at `/files/t{sha}.thumbnail`, posts at `/posts/<id>`.
    e621 notes (Danbooru fork; e926 is the SFW mirror, same DB): search is
    a bare `/posts.json` array, detail `/posts/<id>.json`; tags arrive
    flat (buckets via the shared classifier, species/lore stay native-
    capable); `s/q/e` ratings, `order:score` server-side; descriptive
    UA required, 2 req/s hard limit, auth (`login`+`api_key`) only for
    private actions. AnimePictures notes: `/api/v3/posts` on the `api.`
    subdomain (0-based pages, `search_tag`/`denied_tags`); summaries are
    tagless (HyBooru-style hydration via refresh), detail carries numeric
    tag types (1 character, 4 author, 3/5/6 copyright, else general);
    media URLs reconstructed from md5 (`opreviews`/`oimages`); all-SFW
    (`erotics` level folds to general/sensitive/questionable).
  - `sitequirks.h` = per-host quirks (api subdomain, cookies, PoW and
    Cloudflare browser-only API walls).
  - `boorutypes.h` = `BooruEngine` enum, `BooruSite`, `SiteCreds`,
    `SearchResult`.
- `src/remote/` - QNAM plumbing + batch jobs.
  - `remotesource.h:18` `RemoteSource`: registry (`presetBoorus`,
    `siteFor`, `setConfiguredBoorus`), async `search()` (`:48`), credential
    `probeCredentials`/`testCredentials` (`:52-66`), cookie attach
    `applyCookies`, browser fallback `WebFetchFn` for PoW walls (`:72-79`).
  - `remotesource.h:120` `RemotePosts`: `fsTagForPost` (host-based folder
    tag, `:130`), `hostKeys`, `entryForPost` (downloaded/local rows
    resolve via folder host or legacy label, strays/library null),
    `postPageUrlFor` (engine-shaped post links), `mediaReferer(For)`, `engineForSiteKey`,
    `skipAutoClassify` (`:147`), `foldedBucketFor`, `proxyForUrl/FromSpec`,
    `redactSecrets`, `probeReportText`/`showProbeDialog` (`:170-172`);
    `RefreshTarget` (`:176`) + `resolveRefreshTarget` + `PostRefresher`
    (`:193`, single-post tag refresh).
  - `downloadmanager.h:45` `DownloadManager`: `plan()` (`:51`),
    `completeJob()` (`:95`), `DownloadsDialog` (`:113`). Media accepted by
    header OR magic bytes (`looksLikeMediaBytes`, `src/remote/remotesource.
    cpp`), so empty/misleading types (Shimmie2) download fine while wall
    pages still fail named; failed rows are clickable to their reason.
  - Also: `imagecache` (full-image fetch for viewer), `thumbloader`
    (grid thumbs), `tagsuggest` (per-engine autocomplete: favourites ->
    cached by post count -> remote, merged into one ranked list;
    zero-post rows are dropped at parse for count-reporting engines and
    pruned from the cache on open, so stale aliases/empties never crowd
    out ranked tags), `faviconcache`
    (site icons), `cookiegrab` (WebEngine cookie capture for walled hosts),
    `postmeta` (notes + comments + artist commentary for viewer; rows are
    scoped by `post_id` so an unscoped endpoint can never paint another
    post's notes/comments). Notes sources: Danbooru/e621 `/notes.json`,
    Moebooru `/note.json`, dapi XML (`<note>` table) as fallback, Gelbooru/
    Safebooru public post-page HTML (`<article data-x…>` / `note-box`
    divs; dapi `s=note` answers 401 + empty body anonymously, so the page
    is ALWAYS used there), HyBooru `/api/post/<id>` (`notes[].rect` are
    percentages of the detail `width`/`height`), Szurubooru
    `/api/post/<id>` (`notes[].polygon` is a fractional-canvas point list;
    the overlay is its bounding box, old pixel-valued instances kept);
    no endpoint on Shimmie/Nuuru/Philomena/BooruOnRails/AnimePictures
    (a 404 there would fail the whole fetch).
    The viewer's tag panel shows a `Loading notes and comments…`
    indeterminate strip under TAGS while the per-post fetch is
    outstanding (`ViewerPage::m_metaBusy`), cleared by `done`.
    Parse failures on 2xx bodies report "Unparseable response" (never a
    blank reason). Parts are independent: `finishPart` reports success
    when any requested part answered, so a failed comments leg can never
    discard parsed notes. Plain QNAM goes first even on `browserApi`
    hosts (the meta endpoints answer anonymously where search is
    challenged) and those requests are sent COOKIELESS: a Turnstile
    `cf_clearance` is fingerprint-bound, so presenting it from QNAM can
    trigger the challenge it should avoid. Each part then falls back once
    to the capture profile via `retryPartInBrowser`. On `browserApi` hosts (Donmai/CDN) all three parts
    ride the capture profile via `QtConcurrent` + `webFetcher`, since QNAM
    cannot use a browser-bound `cf_clearance`. Commentary = Danbooru
    `artist_commentary`, Philomena `description` (Philomena/Booru-on-Rails
    detail), HyBooru rect-null Hydrus notes.
- `src/library/` - local data + classification.
  - `scanner.h:19` `Scanner`: recursive scan, both sidecar layouts, emits
    `progress`/`cacheStats`; impl `src/library/scanner.cpp:156`.
  - `metaparser` (`metaparser.h`): sidecar JSON -> `Post` (gallery-dl native/
    flat, LOSN), explicit-rating healing at parse time.
  - `cache.h:18` `ScanCache`: per-root SQLite keyed by (path, mtime, size);
    `dbPathFor()` at `:26`.
  - `index.h:30` `Index`: in-memory posts + tag counts; `query()` (`:38`),
    `suggest()` buckets for colored autocomplete (favourites, then prefix
    matches by post count; substring hits only fill leftover slots),
    `topTags()` seeds the persistent cache, `collectUntyped()` (`:53`),
    `applyTagTypes()` (`:64`); `applyTagBuckets()` (`:91`) = same core for
    browse vectors.
  - `tagcountcache.h` `TagCountCache` (`<data>/tags.db`): tag + bucket +
    post count + `updated` (last tag-API confirmation) learned from every
    tag-API answer (and from the archive after a scan); prefix query
    ordered by count, favourites lifted. Feeds the omnibar instantly,
    before the network answers. Rows a host has not re-confirmed in a
    week (`TagSuggester::kFreshSeconds`) are not used as instant answers;
    the network refills them, so popularity never ages silently. The
    popup paints the count right-aligned (`500`, `7k`, `1.5M`).
  - `queryor.h:16` `QueryOr`: `~(...)` OR-groups - `parse()` (`:28`),
    `expand()` cartesian cap `kMaxCombos = 12` (`:36`), `groupsMatch()`
    (`:40`), `suggestBare()` (`:45`), `mergeActiveTokens()` (`:50`) = the
    Enter/pill commit (negation flips a pill, duplicates ignored).
  - `favdb.h:12` `FavDb` (`store.db`): identity `pidFor`/`siteFor` (`:24`),
    `open/add/addMany/remove/list` (`:33-52`), `unlinked()` (`:61`),
    `setTagSplits()` (`:68`); schema + identity upgrades live in
    `src/migrations/` (`m_favdb_schema`, `m_favdb_hash_ids`).
  - `settingsexport.h` (`src/library/`): JSON backup/restore of
    portable settings + favourites (`exportJson`/`importJson`;
    boorus merge by key, favourites addMany skips duplicates).
  - `tagtypes.h`: `bucketForType/typeForBucket` (`:31`),
    `parseSafebooruXml/parseYandereJson/parseDanbooruTagsJson` (`:36-38`),
    `TagTypeCache` verdict store (`:84`), `TagTypeFetcher` waterfall (`:112`).
  - `explicittags.h`: built-in set (`:20`), `explicitTagsHash()` cache-buster
    (`:110`), `inferExplicit()` (`:122`).
  - `sidecarrefresh.h`: `SidecarRefresh::collect()` (`:24`), `SidecarBatch`
    bulk re-fetch (`:29`).
  - `stray.h:14` `Stray`: `_Stray` folder convention; `strayresolve.h`
    `StrayResolver` + pure `parseBooruPostUrl`/`parseBooruPageHtml`/
    `postFromRaw` (Download/Add Stray link resolution: public API first,
    post-page HTML fallback, generic e621-style shapes); `losnimport`
    (LOSN `store.db` import + the CSV backup row mapping);
    `mediasize` (header-only pixel dimensions of files already on disk,
    used by the CSV export - videos deliberately skipped);
    `cachejanitor` (10 GB cap enforcement +
    per-entry rating/site tracking so "delete NSFW cache" can keep SFW
    files); `cachepolicy` (global + per-booru disk-cache mode, applied to
    thumbnails and full-image cache).
- `tagcategories.h`: per-booru category merge/rename rules
  (`TagCategoryMerge`, source aliases + classic/custom target); consumed
  by the browse sidebar, filter pills and viewer tag groups.
- `src/migrations/`: one-time schema/config/layout upgrades, one file
  per migration (`migrations.h` contract, `runner.h`
  `MigrationRunner::runPending`). Run at boot with a `Running
  migrations…` dialog only when work is pending; applied ids (+ schema
  generation) persist in `settings.ini` (`migrations/done`,
  `migrations/version`), so later boots skip them. Details in
  `docs/MIGRATIONS.md`.
- `src/ui/` - `mainwindow.h:105` `MainWindow` (all tabs, all searches);
  `BrowserTab` session state (`:47-61`); `PendingSite` OR fan-out (`:342-350`).
  - `viewerpage.h:170` `ViewerPage` (same-window viewer; member defs split across `src/ui/viewerpage.cpp`, `viewerpage_mediaviews.cpp` (`ImageView`/`GifView`/`VideoView`), `viewerpage_nav.cpp` navigation/prefetch, `viewerpage_meta.cpp` metadata/notes/comments/commentary popups+reload, `viewerpage_tags.cpp` tag rendering, `viewerpage_load.cpp` notes/cache/web load, `viewerpage_setters.cpp` state/setters/badges, `viewerpage_detail.cpp` Hybooru detail fetch; `ImageView` `:33`,
    `GifView` `:65`, `VideoView` `:93`); software video decoding forced in
    `main.cpp` (`QT_FFMPEG_DECODING_HW_DEVICE_TYPES=,`, user-overridable;
    the VAAPI/hardware path segfaulted on broken surfaces) and
    `VideoFrameItem::setFrame` never maps non-readable frames;
    fullscreen via `MainWindow::setViewerFullscreen` + `setImmersive`;
    video ghost-audio killed by a wanted-source gate (`VideoPane::
    m_wanted`) + mute-on-stop; audio output follows the desktop's default
    sink (`followDefaultAudioDevice`, re-resolved on
    `QMediaDevices::audioOutputsChanged` and on every `setUrl`, so a
    Plasma headphone/speaker switch is honoured mid-session); note click
    opens `showNotePopup`;
    `welcomepage` (home + mascot);
    `metatext` (untrusted DText/BBCode/Markdown -> escaped Qt rich text
    with http(s)/mailto-only links, relative targets resolved per entry;
    used by the commentary popup and comment bodies);
    `prefsdlg` (tabbed preferences; booru editor/detect/probe + swatches/`harvestInto` split to `src/ui/prefsdlg_booru.cpp`); `straydialog` (Download/Add Stray);
    `statsdialog` (Info -> Statistics: favourites/download sizes, cache
    tree, tag vocabulary and ranked tags/artists/characters/series/boorus;
    DB side read on the UI thread, download/cache walk on a worker via
    `QFutureWatcher` in `mainwindow_stats.cpp`);
    `flowlayout` (filter pills), `tabbar` (marquee tabs, browser-style
    drag-reorder: opaque grab shown in a raised overlay child of the
    window, sliding horizontally only, with welcome pinned first),
    `theme`, `uicons`,
    `progressbar`.
- `src/post.h:11` `Post`: shared record (local + remote + fav rows);
  `tagTokenOk/cleanTagTokens` hygiene (`:110-128`), `isAiPost` (`:87`).

## Data flows

- Browse search (`src/ui/mainwindow.cpp`):
  - `commitSearch()` parses query -> AND tokens + OR groups (`QueryOr`);
    `doBrowseSearch()` (`:1831`) fans out per site x per OR-alternative;
    `pumpSiteSearch` queues alternatives; each `RemoteSource::search()` gets
    `translateSearchTags(tokens, engine)` (`:1827`) +
    `sortSearchTag(sort, engine)` (`:1866`); user `sort:/order:` tokens win.
  - Merge: completions accumulate in `PendingSite.posts`, deduped, stamped
    with `siteKey`/`siteShort` badge; `applyBrowseFilter()` (`:2181`) applies
    rating/category/blacklist/OR-group match; `sortPosts()` (`:2205`) for
    client-side sorts.
  - Infinite scroll: near-bottom in browse tab calls `loadNextPage()`
    (`:960-966,1799`); append path skips dupes (`:2113`), throttled vs 429s;
    `Favourites` + `Downloads` join merges as virtual sources
    (`mainwindow.cpp:221`).
  - Downloads media picker (`m_mediaBox`, `onMediaChanged`): All /
    Images / GIFs / Animated / Videos, extension-only. Shown on the
    `Downloads` source and the local `Browse Downloads` tab
    (`MainWindow::isDownloadsTab`); filters download-root rows in
    `applyBrowseFilter`/`applyFilter` (`mediaFilterMatchesExt`,
    `src/post.h`). Rating + sort are shown on browse, favourites and both
    downloads views; local tabs enable every sort order (client-side).
- Downloads (`src/remote/downloadmanager.cpp:115` `plan`,
  `:385` `completeJob`):
  - `plan(posts, destDir)` -> one `DownloadJob` per post
    (`downloadmanager.h:28`): `dest/<host>/<host>_<md5>.<ext>` + `.json`;
    host folder via `fsTagForPost` (`mainwindow.cpp:884,3261`).
  - Media bytes are staged as `<dest>.part` and renamed onto the final
    name only after the content sniff; abort/crash can therefore never
    leave a half-written image under a real filename. The viewer's
    full-res cache is *moved* into the download folder (`QFile::rename`,
    copy fallback). When the viewer is still buffering the exact URL the
    job is parked and the cache fetch pinned (`ImageCache::pin`), so the
    bytes land once and stepping to another post cannot abort them.
  - Every accepted job is journaled in `download_queue.db`
    (`downloadqueue.*`); completion/skip/failure deletes the row. A
    later launch offers Continue / Remove history / Close Box for the
    leftovers. Enqueues while a batch runs append instead of cancelling
    it.
  - Closing the app mid-download asks Abort and quit / Finish current,
    then quit / Finish all, then quit / Keep downloading. Abort deletes
    `.part` files + journal; finish-current leaves the rest of the queue
    in the journal for the next launch, finish-all drains it.
  - `completeJob`: content sniff, md5 hash-and-rename for nameless files,
    pretty-printed raw-API sidecar write (backfilled from the parsed post
    to a full gallery-dl-style record: urls, buckets, dims, ids);
    summary carries `downloaded` for auto-favourite (`downloadmanager.h:35`).
  - Gallery links (Pixiv/Fanbox/Pawchive/…) bypass `DownloadManager`:
    `GalleryDlRunner` (`src/remote/gallerydl.{h,cpp}`) shells out to the
    system gallery-dl (transport only, no metadata flags) into `_Stray/`,
    then writes its own stray sidecars for new files (tags empty for the
    user, `post_url`/`source` = pasted page, never overwrites existing
    sidecars). One flight per URL via `MainWindow::runGalleryDl` /
    `startGalleryFlight` (`src/ui/mainwindow_downloads.cpp`), progress
    view `GalleryDlDialog` in `src/ui/straydialog.{h,cpp}`. No
    auto-favourite on gallery flights (bulk, Favourites is not a bulk
    inbox); Pixiv/Pawchive string-array tags parse as general + untyped
    (`metaparser.cpp:161`).
    LoliSnatcher imports reuse the same folder/name shape via
    `LosnImport::targetPaths` and skip media already on disk by md5
    anywhere below the download root (`LosnImport::diskIndex`) or by the
    file/post URLs recorded in existing sidecars (rows the backup cannot
    hash, hashed-renamed imports), migrating old app-made `<Label>/` files
    once.
    Grid/viewer download marks are filesystem-only (`mainwindow_heal.cpp:102`
    `downloadDiskIndex` + `isPostDownloaded`, md5 then planned filename).
  - Download/Add Stray (`strayresolve.h`): direct media passes through;
    recognised post pages probe their API (`parseBooruPostUrl` ->
    `engineOps()->refreshUrl`), then fall back to page HTML (`og:image` +
    `li.tag-type-*`). Posts are stamped `_Stray` with id 0 (URL-hash
    identity) and carry the API object + `tags_*` buckets in the sidecar;
    `Favourite only` writes the same row to `FavDb` without a media job.
- Post drag/copy (`MainWindow::showGridMenu`, `showViewerContextMenu`,
  `RemotePosts::dragUrlFor`): grid cards carry their drag URL in a model
  role; viewer media starts a `QDrag` at fit zoom. Copy actions cover
  post page, media URL, resolved local path and file-for-clipboard for
  stills/GIFs/videos. `copyPostFilesToClipboard` uses disk/cache copies
  first and pulls remote/favourite-only media into `TempMedia`
  (`/tmp/box-ext`) before setting file URLs on the clipboard.
- Keyboard: window `QShortcut`s (`Ctrl+T/W/Shift+T/D/L/O/Shift+O`,
  `F5`/`Ctrl+R`, F1) live in `MainWindow` (`mainwindow.cpp`); closed tabs
  pile up in `m_closedTabs` for browser-style reopen. Grid `Space`
  toggles selection in `GridView::keyPressEvent`; Enter opens only a
  single selection (`onGridActivated`).
- Fullscreen viewer (`MainWindow::setViewerFullscreen`): hides shell
  chrome and the viewer toolbar/sidebar; F11/Esc/double-click/right-click
  toggle it while arrow navigation keeps working. The exit restores the
  captured `windowState()`; KWin (Wayland) answers an unfullscreen by
  restoring the normal geometry and dropping the maximize flag, so
  `changeEvent` re-issues `showMaximized()` after the compositor's
  configure (an 800 ms timer is the backstop) - without that a
  maximized window came back filling the workspace with a normal frame.
- HyBooru hydration (`MainWindow::scheduleHybooruHydration`): summaries
  carry no tags, so every loaded row is refreshed through a dedicated
  `PostRefresher` queue and merged into the grid/sidebar/viewer.
- Media cache (`src/remote/imagecache.*`): persistent mode writes under
  `baseDir`; "cache nothing" (and browsing without full cache) keeps a
  bounded 512 MB session RAM cache, staged only in `box-ram-<pid>/` and
  destroyed on unload. `prefetchRange` warms adjacent videos with a 2 MB
  Range GET without caching the stream. A URL an explicit download is
  waiting on is pinned against navigation-time `cancelUrl()`.
- Favourites (`src/library/favdb.h`):
  - Identity = site+id (`pidFor`, `:24`); id-less strays key by URL hash,
    pure-local rows by path hash; the `favdb-hash-ids` boot migration
    (`src/migrations/`) heals unstable rows once, `FavDb::open()` keeps a
    cheap probe as a safety net.
  - `relinkFavourites()` (`mainwindow.cpp:2642`, once per launch at `:1485`)
    matches `unlinked()` rows to files on disk via `setLocalPaths`;
    `rowToPost` serves favs as local posts when the file exists
    (keeping the http post URL; strays keep the file), `localPaths`
    feeds the grid/viewer downloaded+favourite marks (downloaded always
    implies the file exists). Grid drops local rows whose file is gone
    instead of carding blanks; the DB rows stay for relink.
    `redownloadMissingFiles()` (File menu) re-pulls missing files that
    still have a source URL - previously-downloaded rows only,
    never-downloaded favourites are never fetched unasked
    (same gate on stray regen).
  - Explicit-rating healing on fav list (`favdb.cpp:467,505`,
    `ratingInferred` flag).
  - Favourites run the same cached classifier as browse tabs
    (`maybeClassifyBrowse`, `mainwindow_classify.cpp:87`): cached verdicts
    apply on tab open, misses fetch once, splits persist via
    `setTagSplits`. `applyTagBuckets` removes a moved tag from every
    bucket first (`index.cpp:338`), so imported duplicate rows cannot
    leave one tag counted in two sections.
  - Sidecar links heal: a `local_path` ending in `.json` is never served
    as media (`FavDb::isSidecarPath` / `mediaPathForSidecar` resolve the
    sibling image/video, stem or flat layout). Startup relink skips
    `.json` files when indexing md5 stems, so the old first-wins bug
    (JSON sorted before PNG, cards black, open-externally launched the
    text editor) cannot recreate itself.
  - `View metadata…` (`mainwindow_metadata.cpp`, `metadatadialog.cpp`)
    reads the sidecar (or in-memory API object) into a summary/tag page
    plus a raw JSON tree; `Show in folder` uses FileManager1 D-Bus with
    per-file-manager `--select` fallbacks (`desktopfiles.h`).
  - Tab switches re-use the rows already held by the tab; only
    `refreshFavTabs()` re-lists `store.db` (every fav/download/import
    path calls it), and `relinkFavourites()` runs once per launch
    (`m_favRelinked`). `keyForLabel` memoises normalised labels per
    configured-registry generation, and `fillModel` caches entry/badge
    pixmap per fill, so a 2000-row favourites tab switches in ~120ms.
  - Activation is skeleton-first (`activateTab`,
    `mainwindow_tabs.cpp:474`): the tab bar/title/stage switch and a
    `GridLoadingOverlay` busy bar paint synchronously, while the DB
    decode, scan or search runs on the next event-loop turn
    (`finishActivateTab`). `hideTabLoading` only drops the veil for the
    owning tab, so a stale scan/search finishing after the user left
    cannot clear the new tab's busy state.
  - Per-tab post identity is `BrowserTab::viewerKey` (site#id or file);
    `finishActivateTab` reopens that post on return. `showViewer`/post
    stepping write it, `backToGrid(false)` (Back, Esc, omnibar, heal)
    clears it so the tab is a grid tab again, and `activateTab` calls
    `backToGrid(true)` to keep the outgoing tab's state. Persisted in
    `tabs/saved*` so an app restart restores the same page.
  - Origin badges resolve favourite labels case-insensitively
    (`keyForLabel`, `remotesource.cpp:940`) and fall back to the post/file
    URL host (`entryForHost`), so a renamed entry keeps its favicon and
    short name.
- Tag classification waterfall (`src/library/tagtypes.h:112` `TagTypeFetcher`):
  Gelbooru-with-creds -> anon Danbooru -> Safebooru -> yandere; misses
  (type -1) cached, transient (`kTransientFailure = -2`) not cached and
  retried (`tagtypes.h:17-18`). A run started while another is active
  MERGES its tags into the running queue instead of cancelling it:
  pagination used to abort the in-flight batch, leaving the first page's
  tags general forever (`fetch()`).
  - Manual overrides (`tagtypes/manual` ini, `normalizeTagOverrides`)
    win without network; `skipAutoClassify` per entry
    (`remotesource.h:147`) leaves meme-host tags as-is; disabled buckets
    (`gelbooru:character` form) fold to general via `foldedBucketFor`;
    streaming `resolved()` verdicts paint the sidebar as a waterfall
    (`mainwindow.h:130-132`, `flushClassifyPartials`); fav splits persist
    via `setTagSplits`. Verdicts also re-push into an open viewer
    (`refreshViewerTags` -> `ViewerPage::refreshCurrentPost`), so a post
    opened mid-batch stops showing its tags as general once the batch
    lands (the panel used to keep the snapshot taken at click time).
- Local scan: `Scanner::scan` (`scanner.cpp:156`) walks recursively, matches
  gallery-dl (`<media>.json`) + LOSN (`<stem>.json`) sidecars,
  `MetaParser` -> `Post`, `inferExplicit` upgrades rating
  (`metaparser.cpp:64,189`), untyped tags -> `unknownOut` for optional
  classification; `ScanCache` (path, mtime, size + `explicitTagsHash`)
  makes rescans incremental.

## Settings + XDG paths

`settingsFilePath()` = `~/.config/box/settings.ini` by default, or
`<BOX_CONFIG_DIR>/settings.ini` / the `[app] configDir` pointer stub
(`src/settings.cpp:10`, bootstrap in `src/main.cpp:82`); defaults in
`AppSettings` (`src/settings.h:19`); `loadSettings`/`saveSettings`
(`src/settings.cpp:25,169`); booru registry under `boorus/order` +
`boorus/<key>` groups (`settings.cpp:265,346`). `saveSettings` is
atomic (tmp-file stage + rename) and rotates the replaced file to
`settings.ini.bak`; `loadSettings` heals a missing/truncated ini from
the backup. Tab add/close/move persist synchronously (`saveTabsNow`);
switches/edits coalesce 1500 ms (`saveTabs`). The Preferences `Move…`
(`src/settings.cpp:36` `moveConfigFiles` + `writeConfigStub`) copies the
ini + `boorus.json` to the target and leaves only the pointer stub.

| Kind    | Path | Notes |
|---|---|---|
| Config  | `~/.config/box/settings.ini` (+ `.bak` last-good backup, `.tmp` stage) | all keys below; `boorus/*` registry; atomic tmp+rename writes |
| Data    | `~/.local/share/box/` (or `storage/dataDir`) | `store.db` favs, `tag_types.db` verdicts, `downloads/`, `download_queue.db` unfinished download journal, `session.lock`, `box.log[.1-.3]` rotating logs (`src/applog.cpp`) (`settings.h:122-167`, `main.cpp:112`) |
| Cache   | `~/.cache/box/` (`appCacheDir`, `settings.cpp:15`) | `roots/` scan index, `thumbs/`, full-image cache, `cache.db`; `clearIndexCache()` drops `roots/` only. Media deletes go through `CacheJanitor::purgeMedia()` (SFW-only keeps provably SFW entries; everything removes `thumbs/`+`full/`) and never touch favourites, downloads, or the scan index. SFW-only still caches explicitly downloaded media (`Post::downloaded`); a favourite with no downloaded file stays filtered. |

Key `settings.ini` groups: `cache/` (index, mode=all|sfw|none, full, limitMB,
thumbSize), `library/` (root, history), `app/` (restoreSession, language),
`fav/` (autoDownload, autoFav, unfavDeletesFile, deleteUnfavs),
`stray/autoFav`, `filter/blacklist`, `classify/enabled`,
`tagtypes/` (manual, customBuckets, categoryMerges, customCategories,
 tagCustomCategories, legacy disabledBuckets), `boorus/*/enabled`, `browse/` (mode, sites, limit),
`tabs/saved*` + `last/*` (session restore, cap 25), `downloads/dir`,
Startup safeguard (`main.cpp`): if a configured `storage/dataDir`, `cache/dir`, or `downloads/dir` does not exist (disk not mounted), the app warns and offers Use defaults / Change folders / Quit, and never creates files at that path.

`storage/dataDir`, `startup/defaultTab`, `window/`, `boorus/` (+ per-entry
`cacheMode`), `explicit/tags`, `theme/` (values are `assets/themes/*.ini` keys),
`home/mascot*` (default embedded `マニフェスト.jpg`), `grid/`, `viewer/`.
Never a group named `General` (QSettings reserves it; see
`src/settings.cpp:39-43`).

## Error handling

- `probe` pattern: raw request/response captured on `RemoteSource`
  (`lastUrl/lastBody/lastHttp/lastCookieNames`, `remotesource.h:37-44`);
  rendered by `probeReportText`, shown in modal inspector
  (`remotesource.h:170-172`); browse failures attach to status bar
  (`m_statusProbeTitle/Text`, `mainwindow.h:327-329`; click opens dialog;
  `mainwindow.cpp:1991,2022-2141`).
- CredentialProbe (`remotesource.h:56-66`) = Test-button diagnostics;
  prefs probe runs on worker thread (`prefsdlg.cpp:1184,1320-1381`).
- Media CDN walls (`cdn.donmai.us` 403 behind Cloudflare,
  `derpicdn.net`): one site captcha unlocks the CDN in a browser, so a
  site clearance stays entry-level and `cookieHeaderValueForHost`
  forwards clearance-only cookies to known same-operator CDNs (never the
  session). CDN-specific captures are host-scoped (`host!name=value`,
  `harvestScopedCookies`) so they cannot clobber the site value; scoped
  entries only apply under their own host, narrower scope wins. A walled
  viewer image registers the failing URL in `m_mediaWalls`; grid
  thumbnails do the same without being opened (`ThumbLoader::thumbnailWall`)
  and retry a walled plain GET through the browser backend. The first
  thumbnail wall per site opens one capture pass at the site root (not
  the CDN URL, which can serve bytes without minting cookies), then the
  visible grid and open viewer retry. Explicit downloads do the same:
  a QNAM HTML/403 result re-runs the job once via the browser backend
  (`DownloadJob::webTried`). `TempMedia` (open externally/clipboard)
  rejects challenge pages instead of saving them as media. The silent
  clearance refresh loads the failing media URL, not just the site root;
  `proxyForUrl` follows the same CDN matching.
- Transient retry: `isTransientSearchError` = rate-limit/timeout/5xx, never
  auth/wall/bad-filter (`booruengine.h:42`); one delayed auto-retry per fresh
  search (`m_searchRetried`, `mainwindow.h:363`; `mainwindow.cpp:2151-2159`);
  appends never retry; TagType transient failures uncached
  (`tagtypes.h:17-18`); `SidecarBatch` per-file failures silent + counted
  (`sidecarrefresh.h:3-6`).

## i18n

UI strings `tr()`-wrapped (English source). `CMakeLists.txt:63-89`:
`update_translations` target refreshes `.ts`; translators copy
`translations/box.ts` -> `translations/box_<locale>.ts`; `.qm` embeds under
`:/i18n` and auto-loads (`src/main.cpp:61-87`; prefs `app/language` pin or
system locale). `tests/check_translations.py:1-68` enforces per-locale:
non-empty translations, `%1..%9` placeholder match, `&` accelerator count,
`language` attribute present.
