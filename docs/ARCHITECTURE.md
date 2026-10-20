# ARCHITECTURE — box (booru client, Qt6 C++)

Native C++/Qt6 client for local image archives + online boorus. Local
archives are read-only; downloads + favourites feed back into offline browsing.
Entry: `src/main.cpp:40` (`box`, single-instance via `session.lock`).

- Process/thread model: one GUI-thread process (second copy exits,
  `src/main.cpp:112`).
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

- `src/engines/` — per-engine API shapes behind `BooruEngineOps`
  (`src/engines/booruengine.h:74`: `searchUrl`/`parseSearch`/`refreshUrl`/
  `parseSingle`/`applyAuth`; `engineOps()` at `:121`).
  - Pure shared helpers in `RemotePosts`: `translateSearchTags`,
    `sortSearchTag`, `isTransientSearchError`, `fromDanbooru/Gelbooru/Moebooru/
    Szuru/Shimmie`, `parseShimmieXml` (`src/engines/booruengine.h:26-60`).
  - `sitequirks.h` = per-host quirks (api subdomain, cookies, PoW walls).
  - `boorutypes.h` = `BooruEngine` enum, `BooruSite`, `SiteCreds`,
    `SearchResult`.
- `src/remote/` — QNAM plumbing + batch jobs.
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
    `completeJob()` (`:95`), `DownloadsDialog` (`:113`).
  - Also: `imagecache` (full-image fetch for viewer), `thumbloader`
    (grid thumbs), `tagsuggest` (per-engine autocomplete), `faviconcache`
    (site icons), `cookiegrab` (WebEngine cookie capture for walled hosts),
    `postmeta` (notes + comments fetch for viewer).
- `src/library/` — local data + classification.
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
  - `queryor.h:16` `QueryOr`: `~(...)` OR-groups — `parse()` (`:28`),
    `expand()` cartesian cap `kMaxCombos = 12` (`:36`), `groupsMatch()`
    (`:40`), `suggestBare()` (`:45`).
  - `favdb.h:12` `FavDb` (`store.db`): identity `pidFor`/`siteFor` (`:24`),
    `open/add/addMany/remove/list` (`:33-52`), `unlinked()` (`:61`),
    `setTagSplits()` (`:68`); `migrateHashIds()` (`:74`).
  - `tagtypes.h`: `bucketForType/typeForBucket` (`:31`),
    `parseSafebooruXml/parseYandereJson/parseDanbooruTagsJson` (`:36-38`),
    `TagTypeCache` verdict store (`:84`), `TagTypeFetcher` waterfall (`:112`).
  - `explicittags.h`: built-in set (`:20`), `explicitTagsHash()` cache-buster
    (`:110`), `inferExplicit()` (`:122`).
  - `sidecarrefresh.h`: `SidecarRefresh::collect()` (`:24`), `SidecarBatch`
    bulk re-fetch (`:29`).
  - `stray.h:14` `Stray`: `_Stray` folder convention; `losnimport`
    (LOSN `store.db` import); `cachejanitor` (10 GB cap enforcement).
- `src/ui/` — `mainwindow.h:105` `MainWindow` (all tabs, all searches);
  `BrowserTab` session state (`:47-61`); `PendingSite` OR fan-out (`:342-350`).
  - `viewerpage.h:170` `ViewerPage` (same-window viewer; `ImageView` `:33`,
    `GifView` `:65`, `VideoView` `:93`); software video decoding forced in
    `main.cpp` (`QT_FFMPEG_DECODING_HW_DEVICE_TYPES=,`, user-overridable;
    the VAAPI/hardware path segfaulted on broken surfaces) and
    `VideoFrameItem::setFrame` never maps non-readable frames;
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
    still have a source URL — previously-downloaded rows only,
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
| Cache   | `~/.cache/box/` (`appCacheDir`, `settings.cpp:15`) | `roots/` scan index, `thumbs/`, full-image cache, `cache.db`; `clearCaches()` drops `roots/`+`thumbs/` (`settings.cpp:250`) |

Key `settings.ini` groups: `cache/` (index, thumbs, full, limitMB,
thumbSize), `library/` (root, history), `app/` (restoreSession, language),
`fav/` (autoDownload, autoFav, unfavDeletesFile, deleteUnfavs),
`stray/autoFav`, `filter/blacklist`, `classify/enabled`,
`tagtypes/` (manual, disabledBuckets), `browse/` (mode, sites, limit),
`tabs/saved*` + `last/*` (session restore, cap 25), `downloads/dir`,
`storage/dataDir`, `startup/defaultTab`, `window/`, `boorus/`,
`explicit/tags`, `theme/`, `home/mascot*`, `grid/`, `viewer/`.
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
