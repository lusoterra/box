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
    Szuru/Shimmie/Hybooru/Nuuru/Hiddenbooru`, `parseShimmieXml`
    (`src/engines/booruengine.h:25-95`).
    HiddenBooru notes: shapes from the v0.8 source tarball (Tor-first
    instance + I2P mirror; plain fetches need the Tor stack - or i2pd,
    SOCKS5 at `socks5h://127.0.0.1:4447`); `terms=`
    search with `id:` refresh narrowing, `/api/tag/complete` suggestions,
    letter ratings, relative file URLs; md5 normalized into `rawJson`
    when the hash is md5. The API sits behind an HBS anti-bot wall (302
    to `/hbs/verify`, captcha clearance cookie): QNAM reports the wall
    and the browser-backed stack retries it, so the embedded browser
    must ride the same proxy (`applyWebEngineProxy`, `--proxy-server`
    at startup) or onion hosts fail it with ERR_NAME_NOT_RESOLVED.
    Nuuru notes: plain fetches hit HTTP 449 `{"error":"pow_required"}`
    (or `captcha_required`: hCaptcha via `/api/captcha/*`); PoW is a WASM
    worker (`/api/pow/challenge` → `/api/pow/verify`). The token embeds
    the solving egress IP, so the app solves PoW natively on the searching
    QNAM (`PowSolver`: same proxy/egress/jar, one retry, tokens merged back
    into the entry) instead of relying on browser-harvested cookies that die
    on any IP change. Only captchas still need the human browser path.
    Traffic rides the browser-backed stack (`powWall` quirk on
    `soybooru.com`);
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
  - `sitequirks.h` = per-host quirks (api subdomain, cookies, PoW walls).
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
    (grid thumbs), `tagsuggest` (per-engine autocomplete), `faviconcache`
    (site icons), `cookiegrab` (WebEngine cookie capture for walled hosts),
    `postmeta` (notes + comments + Danbooru artist commentary for viewer).
- `src/library/` - local data + classification.
  - `scanner.h:19` `Scanner`: recursive scan, both sidecar layouts, emits
    `progress`/`cacheStats`; impl `src/library/scanner.cpp:156`.
  - `metaparser` (`metaparser.h`): sidecar JSON -> `Post` (gallery-dl native/
    flat, LOSN), explicit-rating healing at parse time.
  - `cache.h:18` `ScanCache`: per-root SQLite keyed by (path, mtime, size);
    `dbPathFor()` at `:26`.
  - `index.h:30` `Index`: in-memory posts + tag counts; `query()` (`:38`),
    `suggest()` buckets for colored autocomplete, `collectUntyped()` (`:53`),
    `applyTagTypes()` (`:64`); `applyTagBuckets()` (`:91`) = same core for
    browse vectors.
  - `queryor.h:16` `QueryOr`: `~(...)` OR-groups - `parse()` (`:28`),
    `expand()` cartesian cap `kMaxCombos = 12` (`:36`), `groupsMatch()`
    (`:40`), `suggestBare()` (`:45`).
  - `favdb.h:12` `FavDb` (`store.db`): identity `pidFor`/`siteFor` (`:24`),
    `open/add/addMany/remove/list` (`:33-52`), `unlinked()` (`:61`),
    `setTagSplits()` (`:68`); `migrateHashIds()` (`:74`). Also owns the
    opt-in `search_history` table (`addSearch/searches/clearSearches`).
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
    (LOSN `store.db` import); `cachejanitor` (10 GB cap enforcement +
    per-entry rating/site tracking so "delete NSFW cache" can keep SFW
    files); `cachepolicy` (global + per-booru disk-cache mode, applied to
    thumbnails and full-image cache).
- `tagcategories.h`: per-booru category merge/rename rules
  (`TagCategoryMerge`, source aliases + classic/custom target); consumed
  by the browse sidebar, filter pills and viewer tag groups.
- `src/ui/` - `mainwindow.h:105` `MainWindow` (all tabs, all searches);
  `BrowserTab` session state (`:47-61`); `PendingSite` OR fan-out (`:342-350`).
  - `viewerpage.h:170` `ViewerPage` (same-window viewer; `ImageView` `:33`,
    `GifView` `:65`, `VideoView` `:93`); software video decoding forced in
    `main.cpp` (`QT_FFMPEG_DECODING_HW_DEVICE_TYPES=,`, user-overridable;
    the VAAPI/hardware path segfaulted on broken surfaces) and
    `VideoFrameItem::setFrame` never maps non-readable frames;
    fullscreen via `MainWindow::setViewerFullscreen` + `setImmersive`;
    video ghost-audio killed by a wanted-source gate (`VideoPane::
    m_wanted`) + mute-on-stop; note click opens `showNotePopup`;
    `welcomepage` (home + mascot);
    `prefsdlg` (tabbed preferences); `straydialog` (Download/Add Stray);
    `flowlayout` (filter pills), `tabbar` (marquee tabs, drag-reorder
    with welcome pinned first), `theme`, `uicons`,
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
    `♥ Favourites` + `⭳ Downloads` join merges as virtual sources
    (`mainwindow.cpp:221`).
- Downloads (`src/remote/downloadmanager.cpp:115` `plan`,
  `:385` `completeJob`):
  - `plan(posts, destDir)` -> one `DownloadJob` per post
    (`downloadmanager.h:28`): `dest/<host>/<host>_<md5>.<ext>` + `.json`;
    host folder via `fsTagForPost` (`mainwindow.cpp:884,3261`).
  - `completeJob`: content sniff, md5 hash-and-rename for nameless files,
    pretty-printed raw-API sidecar write (backfilled from the parsed post
    to a full gallery-dl-style record: urls, buckets, dims, ids);
    summary carries `downloaded` for auto-favourite (`downloadmanager.h:35`).
  - Download/Add Stray (`strayresolve.h`): direct media passes through;
    recognised post pages probe their API (`parseBooruPostUrl` ->
    `engineOps()->refreshUrl`), then fall back to page HTML (`og:image` +
    `li.tag-type-*`). Posts are stamped `_Stray` with id 0 (URL-hash
    identity) and carry the API object + `tags_*` buckets in the sidecar;
    `Favourite only` writes the same row to `FavDb` without a media job.
- Post drag/copy (`MainWindow::showGridMenu`, `showViewerContextMenu`,
  `RemotePosts::dragUrlFor`): grid cards carry their drag URL in a model
  role; viewer media starts a `QDrag` at fit zoom. Copy actions cover
  post page, media URL and resolved local path for stills/GIFs/videos.
- Fullscreen viewer (`MainWindow::setViewerFullscreen`): hides shell
  chrome and the viewer toolbar/sidebar; F11/Esc/double-click/right-click
  toggle it while arrow navigation keeps working.
- HyBooru hydration (`MainWindow::scheduleHybooruHydration`): summaries
  carry no tags, so every loaded row is refreshed through a dedicated
  `PostRefresher` queue and merged into the grid/sidebar/viewer.
- Media cache (`src/remote/imagecache.*`): persistent mode writes under
  `baseDir`; "cache nothing" (and browsing without full cache) keeps a
  bounded 512 MB session RAM cache, staged only in `box-ram-<pid>/` and
  destroyed on unload. `prefetchRange` warms adjacent videos with a 2 MB
  Range GET without caching the stream.
- Favourites (`src/library/favdb.h`):
  - Identity = site+id (`pidFor`, `:24`); id-less strays key by URL hash,
    pure-local rows by path hash; `migrateHashIds()` heals unstable rows
    (`favdb.cpp:80`).
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
- Tag classification waterfall (`src/library/tagtypes.h:112` `TagTypeFetcher`):
  Gelbooru-with-creds -> anon Danbooru -> Safebooru -> yandere; misses
  (type -1) cached, transient (`kTransientFailure = -2`) not cached and
  retried (`tagtypes.h:17-18`).
  - Manual overrides (`tagtypes/manual` ini, `normalizeTagOverrides`)
    win without network; `skipAutoClassify` per entry
    (`remotesource.h:147`) leaves meme-host tags as-is; disabled buckets
    (`gelbooru:character` form) fold to general via `foldedBucketFor`;
    streaming `resolved()` verdicts paint the sidebar as a waterfall
    (`mainwindow.h:130-132`, `flushClassifyPartials`); fav splits persist
    via `setTagSplits`.
- Local scan: `Scanner::scan` (`scanner.cpp:156`) walks recursively, matches
  gallery-dl (`<media>.json`) + LOSN (`<stem>.json`) sidecars,
  `MetaParser` -> `Post`, `inferExplicit` upgrades rating
  (`metaparser.cpp:64,189`), untyped tags -> `unknownOut` for optional
  classification; `ScanCache` (path, mtime, size + `explicitTagsHash`)
  makes rescans incremental.

## Settings + XDG paths

`settingsFilePath()` = `~/.config/box/settings.ini`
(`src/settings.cpp:10`); defaults in `AppSettings`
(`src/settings.h:19`); `loadSettings`/`saveSettings`
(`src/settings.cpp:25,169`); booru registry under `boorus/order` +
`boorus/<key>` groups (`settings.cpp:265,346`). `saveSettings` is
atomic (tmp-file stage + rename) and rotates the replaced file to
`settings.ini.bak`; `loadSettings` heals a missing/truncated ini from
the backup. Tab add/close/move persist synchronously (`saveTabsNow`);
switches/edits coalesce 1500 ms (`saveTabs`).

| Kind    | Path | Notes |
|---|---|---|
| Config  | `~/.config/box/settings.ini` (+ `.bak` last-good backup, `.tmp` stage) | all keys below; `boorus/*` registry; atomic tmp+rename writes |
| Data    | `~/.local/share/box/` (or `storage/dataDir`) | `store.db` favs, `tag_types.db` verdicts, `downloads/`, `session.lock`, `box.log[.1-.3]` rotating logs (`src/applog.cpp`) (`settings.h:122-167`, `main.cpp:112`) |
| Cache   | `~/.cache/box/` (`appCacheDir`, `settings.cpp:15`) | `roots/` scan index, `thumbs/`, full-image cache, `cache.db`; `clearIndexCache()` drops `roots/` only. Media deletes go through `CacheJanitor::purgeMedia()` (SFW-only keeps provably SFW entries; everything removes `thumbs/`+`full/`) and never touch favourites, downloads, or the scan index. SFW-only still caches explicitly downloaded media (`Post::downloaded`); a favourite with no downloaded file stays filtered. |

Key `settings.ini` groups: `cache/` (index, mode=all|sfw|none, full, limitMB,
thumbSize), `library/` (root, history), `app/` (restoreSession, language),
`fav/` (autoDownload, autoFav, unfavDeletesFile, deleteUnfavs),
`stray/autoFav`, `filter/blacklist`, `classify/enabled`,
`tagtypes/` (manual, customBuckets, categoryMerges, customCategories,
 tagCustomCategories, legacy disabledBuckets), `boorus/*/enabled`, `browse/` (mode, sites, limit),
`tabs/saved*` + `last/*` (session restore, cap 25), `downloads/dir`,
`storage/dataDir`, `startup/defaultTab`, `window/`, `boorus/` (+ per-entry
`cacheMode`), `explicit/tags`, `theme/` (values are `themes/*.ini` keys),
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
- `CredentialProbe` (`remotesource.h:56-66`) = Test-button diagnostics;
  prefs probe runs on worker thread (`prefsdlg.cpp:1184,1320-1381`).
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
