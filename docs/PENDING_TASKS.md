# Pending tasks (from user goal)

## 38. Confirmation for mass actions - DONE
- [x] Multi-post `Download` (context menu, Ctrl+D, Download button,
      stray link lists) asks for a count + destination confirmation
      centrally in `downloadPosts`; paths that already prompted
      (`Redownload missing files…`) pass `confirmBulk=false`.
- [x] Multi-row `Unfavourite` asks before dropping the rows and warns
      when the settings also delete the downloaded files.
- [x] Multi-file `Delete from disk…` already had its permanent-delete
      warning (count + unrecoverable rows), kept as-is.
- Verify: full `ctest` + GUI eyes; confirmations are modal QMessageBoxes.

## 37. Tab labels clipped by the selected bold font - DONE
- [x] `MarqueeTabBar::tabSizeHint` adds the bold-vs-normal title width
      (plus 2px slack) to every tab, so selecting one does not cut its
      last letters and does not resize the whole row.
- [x] Paint/overflow measurement now uses the actual (bold for selected)
      font, so the marquee starts exactly when it is needed.
- Verify: `test_tabbar` (new `selectedTitleFitsBoldWidth`), full `ctest`.

## 36. JSON sidecar healing + metadata viewer + show in folder - DONE
- [x] Favourites relink no longer indexes sidecars: both
      `Site_md5.json` and `Site_md5.png` share a stem, and the JSON used
      to win the first-wins map (local_path became the sidecar, so cards
      stayed black and "Open file" launched the text editor). Sidecars
      are skipped; stale sidecar links heal in memory to the sibling
      media (`FavDb::mediaPathForSidecar`) and are re-linked on startup.
- [x] `View metadata…` on grid and viewer context menus: summary fields
      + tag buckets + raw JSON tree, with `Open JSON in editor…` (new
      `app/defaultTextEditor` preference, ask before using the system
      default when unset) and `Show in folder`.
- [x] `Show in folder` uses the freedesktop FileManager1 D-Bus interface
      (Dolphin/KDE, Nautilus/GNOME), then per-file-manager `--select`,
      then opens the plain folder.
- Verify: `test_favdb` (new: sidecar link heals to media, flat sidecar
      path), `test_gridwindow`; full `ctest`.

## 35. Favourites walls: grid thumbs, capture pass, external open - DONE
- [x] `ThumbLoader` plain GETs that answer a challenge (HTML/JSON,
      401/403/429/449/503) retry through the browser backend even on
      hosts without a `powWall` quirk (Danbooru/cdn.donmai.us), so a
      card no longer waits for a click to appear.
- [x] The first walled thumb per site opens one capture pass
      (`thumbnailWall` -> `onThumbWall`, 5-minute loop breaker); the
      pass starts at the site root, not a CDN URL that may serve the
      image without minting cookies.
- [x] Capturing a clearance in Preferences or the Solve dialog retries
      the visible grid and open viewer immediately.
- [x] `TempMedia` validates magic bytes/content type before writing:
      challenge/login pages are never saved as `.jpg` and opened as
      "the JSON"; `Open externally` retries walled media through the
      browser backend.
- [x] Explicit downloads re-run a walled QNAM job once through the
      browser backend (`DownloadJob::webTried`) instead of failing on
      a Cloudflare CDN.
- [x] Favourites rows resolve their site key from the label before
      stamping thumb requests, so wall detection names the entry.
- Verify: `test_thumbloader` (new: browser fallback on a plain 403,
      wall signal when the browser cannot pass, challenge page rejected
      by `TempMedia`), full `ctest`.

## 34. Hover tooltips on the omnibar dropdowns - DONE
- Rating, source/category, downloads media and sort combos now carry
  `setToolTip` text explaining what they filter/sort and where it runs
  (server vs client-side). Downloads media tooltip lists the extension
  groups (`src/ui/mainwindow.cpp`).
- Verify: `test_gridwindow` still builds; tooltips are static strings.

## 33. cdn.donmai.us 403 after a browser captcha - DONE
- [x] Browser parity: one site captcha unlocks the CDN, so a clearance
      captured on the site stays entry-level and is forwarded to
      `cdn.donmai.us`/`derpicdn.net` (clearance-only; never the session).
      CDN-specific captures stay host-scoped so they cannot clobber the
      site clearance.
- [x] Media failures register the failing URL in `m_mediaWalls`, so the
      Solve button opens the CDN challenge (not just the site root), and
      the silent clearance refresh loads that media URL.
- [x] Manual/silent/prefs captures merge instead of replacing the entry's
      cookie list; the cookie editor accepts `host!name=value`.
- [x] Known media CDNs also inherit the entry's proxy (clearance is
      IP-bound, so the capture browser and QNAM must egress the same way).
- [x] Walled viewer stills/GIFs also retry through the browser backend
      (matching UA + capture-profile cookies), covering Cloudflare/CDN
      403s where QNAM's request fingerprint is rejected even with a
      valid clearance.
- Verify: `test_remotesource` (`cookieHostScoping`, `cookiesApply`,
  `proxyForUrl`, `harvestScopedCookies`), full `ctest`; live CDN
  challenge observed against `cdn.donmai.us` (`cf-mitigated: challenge`).

## 32. Rating + sort back on Favourites and Downloads - DONE
- [x] `m_ratingBox` + `m_sortBox` visible on favourites, the local
      `File -> Browse Downloads` tab, and the virtual `⭳ Downloads` source
      (`setBrowseUi`, `MainWindow::isDownloadsTab`).
- [x] Local tabs enable every sort order (client-side `Index::query`/
      `sortPosts`, no append reshuffle); rating filters the DB list/local
      index. Favourites activation refreshes the sort menu and rating
      items (`rebuildCombos`), so a restored favourites-only session is
      not stuck with just `All`.
- [x] The downloads media picker also follows the local Browse Downloads
      tab and filters its rows client-side (`applyFilter`).
- Verify: `test_gridwindow` (favourites tab exposes rating/sort, all sort
  orders enabled), `test_mediafilter`, full `ctest`.

## 31. Downloads media-type picker - DONE
- [x] Downloads-only media dropdown next to the omnibar: `All media`,
      `Images`, `GIFs` (animated stills: gif/animated webp/jxl),
      `Anything animated` (GIFs + mp4/webm), `Videos`.
- [x] File-extension based only (filesystem truth), never tags or MIME
      types; hidden/ignored on every other tab.
- [x] Local grid filtering happens client-side on the loaded download
      rows (no rescan), and the choice persists for the tab session.
- Verify: `test_mediafilter` (new: extension mapping), `test_gridwindow`
  (GUI boot + tabs), GUI eyes for a Downloads tab with mixed extensions;
  full `ctest` after the build.

## 30. Artist commentary: compact section, readable popup, rich text, more engines - DONE
- [x] Panel: ARTIST COMMENTARY moves to the bottom of the meta tail
      (`src/ui/viewerpage_meta.cpp:renderMetaSections`), rendered as a
      small clickable preview (plain text, capped + ellipsised) instead of
      inline full text.
- [x] Click opens a larger `Artist commentary` dialog with scrollable,
      selectable rich text and clickable links (`showCommentaryPopup`).
- [x] Formatting helper `MetaText::toHtml/toPlain` (`src/ui/metatext.*`):
      DText/BBCode (`[b] [i] [u] [s] [url=] [quote] [spoiler] [code]`),
      Philomena Markdown (`**bold**`, `*italic*`, `~~strike~~`,
      `[label](url)`, headings, quotes, bullets), bare URL auto-linking,
      relative links resolved against the entry base URL. Everything is
      HTML-escaped first; hrefs are http(s)/mailto only.
- [x] Engines with a commentary-like field: Danbooru `artist_commentary`
      (existing), Philomena `description` (Derpibooru v1 / Twibooru v3
      single-image endpoints), HyBooru Hydrus notes (rect-null notes;
      translated commentary preferred). Gelbooru/Safebooru, Moebooru,
      Szurubooru, Shimmie and Nuuru expose no such field (no-op).
- [x] Comment bodies also go through the formatter (DText links/italics),
      notes stay escaped plain text.
- Verify: `test_metatext` (new), `test_postmeta` (new URL/shape cases),
      `test_metathread` (panel tail still lays out), full `ctest`;
      GUI eyes under Xvfb for the preview click and popup link opening.

## 29. Browser shortcuts + favourite clipboard parity - DONE
- Window shortcuts (`src/ui/mainwindow.cpp`): `Ctrl+T` new default tab
  (Default tab booru, else default boorus), `Ctrl+W` close tab,
  `Ctrl+Shift+T` reopen through a LIFO stack of closed tabs,
  `Ctrl+D` download the viewer post / grid selection / current post,
  `Ctrl+L` focus the omnibar, `Ctrl+O` open the post page,
  `Ctrl+Shift+O` open the media in the configured default viewer,
  `F5`/`Ctrl+R` reload, `F1` help.
- Grid keys (`GridView::keyPressEvent`, `onGridActivated`): Space toggles
  the current card selection; Enter opens only a single-selection post and
  does nothing with several selected.
- Closed-tab stack (`MainWindow::onTabClosed/reopenClosedTab`): clones the
  `BrowserTab` state (kind, root, sites, filters, viewer key), drops the
  heavy result vectors (rescan/re-search on reopen), cap 20.
- Help dialog lists every new key; F1 opens it with a `Documentation…`
  button that launches the repository.
- Grid and viewer right-click menus add `Copy file (clipboard)` for
  remote/favourite-only posts: local files go straight to the clipboard,
  missing media is fetched to `TempLocation/box-ext` first
  (`TempMedia::ensureTemp`, `/tmp` on Linux). Viewer also gains `Open file`
  for remote-only rows through the same temp path.
- Verify: `ctest -R gridwindow` (new `tabShortcutsCloseReopenTabs`,
  `spaceTogglesGridSelection`), `check_translations`; GUI eyes for the
  async clipboard download and external viewer launch.

## 28. Code review: warnings, bugs, security hardening - DONE
- Warning sweep (`-Wall -Wextra -Wpedantic`, syntax-only over all 85
  `src` TUs + every `tests` TU): exactly 3 warnings, all fixed by
  changing code, none suppressed. `src/main.cpp` unused `chgBtn`
  (now the dismiss path is explicit: closing the storage dialog
  proceeds instead of re-opening a folder chooser); `tempmedia.cpp`
  vestigial `namFor(url)` parameter removed; `mainwindow_misc2.cpp`
  dead `browseTotal` counter removed.
- Stale scan no longer repaints the wrong tab: `onScanFinished` takes
  the scanned root + posts and stashes into the owning tab when the
  user has moved on (cached tabs start no scan of their own), instead
  of overwriting the displayed tab's rows (`mainwindow_scan.cpp`).
- Tab-close search UAF: `onTabClosed` cancels sources and clears
  `m_searchTab`/`m_pendingSites`/`m_pendingSearches`/`m_loadingPage`
  when the closed tab owns the in-flight search (`mainwindow_tabs.cpp`).
- Deferred `finishActivateTab` captures the tab pointer (verified via
  `indexOf`) instead of the numeric index, so closing a tab first
  can't misdirect activation.
- Szuru `rootIdPosts` is recomputed when the entry's host changes
  (`prefsdlg_booru.cpp`); trailing-slash base URLs no longer produce
  `//post/` / `//api/post/` links (`booruengine.cpp`,
  `engine_szurubooru.cpp`); `relinkFavourites` indexes stems by md5
  suffix (O(1) lookup, same matches) instead of O(unlinked x stems).
- Hardening: external-player command substitutes the path as one argv
  element (spaces/flags can't smuggle); `source` links are http(s)-gated
  like post/file links; cache/temp writes stage via `QTemporaryFile` +
  rename instead of predictable-path Truncate; `Index::query` page math
  is 64-bit (huge page = empty, `INT_MAX` perPage still means all).
- Checked and clear (no change): path traversal/zip-slip, SQL
  (bound values), XXE, SSRF gating, XSS escaping, TLS, secrets in logs,
  PoW budget cap, symlink handling.
- Verify: full rebuild `BUILD:0`; `ctest` 38/38 (incl. new
  `TestIndex::hugePageNeverWraps`).

## 27. Auto-refresh on popup close - DONE
- Popups that mutate favourites/downloads (LoliSnatcher import, Download/Add
  Stray) now re-list favourites, invalidate the download index and repaint
  fav marks when they close, instead of leaving the grid stale until F5.
- `importLosnBackup` connects `QDialog::finished` to `refreshFavTabs()` +
  `invalidateDownloadIndex()` + `repaintFavMarks()` (`src/ui/mainwindow_backup.cpp`).
- `openStrayDialog` does the same on close (`src/ui/mainwindow_stray.cpp`).
- Verify: `make`/`ctest` (38/38); GUI eyes for the fav tab populating after
  closing the import dialog.

## 26. Tab click: instant switch with a loading veil - DONE
- `activateTab` already switched the tab bar/title/stage and deferred
  the heavy work, but the only busy cue was the status-bar bar, outside
  the area the click changes. Added `GridLoadingOverlay`
  (`src/ui/gridloadingoverlay.{h,cpp}`): a centred indeterminate
  progress bar + `Loading <tab>…` label over the grid viewport.
- It is shown and painted synchronously at the end of `activateTab`
  (`showFor` sets geometry, raises, repaints) so the click is visibly
  honoured before the queued `finishActivateTab`/scan/search runs.
- The veil drops when rows actually land: favourites/sync paths and
  cached local/browse paths in `finishActivateTab`, `onScanFinished`,
  and both remote-search completion paths. `hideTabLoading(owner)`
  ignores stale jobs from a tab the user already left.
- Verify: `test_gridwindow` (`tabClickShowsLoadingStateImmediately`)
  asserts the veil is visible the moment `openFavTab`/`onTabChanged`
  returns and hidden once the model has rows; favourites perf test still
  passes.

## 25. Keyboard shortcuts dialog renders newlines - DONE
- The Help → `Keyboard shortcuts…` text had escaped `\\n` in the source,
  so the box showed literal `\n`. Now a real multi-line cheatsheet split
  into Grid / Viewer / Window groups (`src/ui/mainwindow.cpp`).
- Verify: `test_gridwindow` still builds/runs the GUI path; dialog text
  is static, `check_translations` counts the new string as untranslated
  like every new source string.

## 24. About: contact email + PGP fingerprint - DONE
- `showAbout` adds `lev@lusosterone.net` (mailto link) and the PGP
  fingerprint `13EA3CFB9ABC093823509BD668C62637ED987692`
  (`src/ui/mainwindow_prefs.cpp`).
- Verify: `docs/UI-TOUR.md` 07/08 updated; dialog-only change.

## 23. More thumbnail grid ratios - DONE
- Preferences → Appearance → Grid `Thumbnail ratio` now offers 5:4,
  21:9, 4:5, 2:3 and 9:16 alongside 1:1, 4:3, 3:2, 16:9 and 3:4; any
  other value still falls back to the `Custom` entry
  (`src/ui/prefsdlg.cpp`).
- Verify: existing aspect plumbing unchanged (`setAspect`); grid rebuild
  path exercised by `test_gridwindow` (`noFloatingAspectCombo`).

## 22. Favourites tab switch cost - DONE
- Root causes on a 2000-row favourites tab: every visit re-decoded the
  whole DB (`FavDb::list`), `relinkFavourites` stat'ed every linked file
  again, `keyForLabel` compiled a `QRegularExpression` per grid row /
  sidebar post, each card re-resolved its entry + favicon pixmap, and
  the model emitted one layout pass per `appendRow`.
- Fixes: `finishActivateTab` re-uses a tab's already-loaded rows
  (`refreshFavTabs` still re-lists on every DB change); relink runs once
  per launch (`m_favRelinked`); `normalizedLabel` is hand-rolled and
  `keyForLabel` memoises per configured registry generation; fillModel
  skips dedupe keys for favourites rows (PK-unique), caches entry and
  badge pixmap per fill, and blocks model signals into one
  `layoutChanged` (`src/ui/mainwindow_tabs.cpp`, `mainwindow_fill.cpp`,
  `mainwindow_relink.cpp`, `src/remote/remotesource.cpp`).
- Measured (offscreen, 2000 linked favourites): first open 1490 -> ~210ms,
  revisit 1686 -> ~120-145ms.
- Verify: `test_gridwindow` (`favouriteTabSwitchStaysFast`) asserts the
  first open < 1200ms and revisits < 700ms with 2000 favourites.

## 21. Sidebar recategorise reloads the pane - DONE
- Right-click → `Recategorise…` now re-runs the active filter
  (`applyFilter`, which refills the grid, pills and browse sidebar) and
  rescopes the local sidebar too (`rebuildBrowseSidebar`), so the moved
  tag lands in its target section in the same action.
- Categories holding zero or one tag are hidden (classic + custom, browse
  and local sidebars), so moving a tag out of a two-tag category leaves a
  singleton and the section disappears immediately
  (`src/ui/mainwindow_sidebar.cpp`, `mainwindow_sidebar2.cpp`).
- Verify: full `ctest` 38/38; hiding follows the existing section
  visibility rule used for empty buckets.

## 20. Origin badges: favourites + single-booru rule - DONE
- Favourite rows linked to a downloaded copy (`isRemote=false`) resolve
  their origin through the post/file URL host when the stored label is a
  generic engine name no entry answers to (e.g. self-hosted Szuru
  imported as `Szurubooru`), so glegle.gallery keeps its badge
  (`src/remote/remotesource.cpp:1066`).
- Badges are omitted on a plain single-booru tab; favourites, downloads
  and multibooru tabs keep them (`src/ui/mainwindow_fill.cpp`).
- Verify: `test_remotesource` (`badgeHostFallbacks` downloaded-row case);
  full `ctest` 38/38.

## 19. Thumbnail ratio lives only in Preferences - DONE
- The omnibar aspect combo was orphaned (parented to the window, never
  added to a layout) and floated at (0,0) showing the current `3:4`.
  Removed it and its handler; the Preferences → Appearance `Grid` ratio
  remains the single control (`src/ui/mainwindow.cpp`, `mainwindow_misc.cpp`,
  `mainwindow_state.cpp`).
- Verify: `test_gridwindow` (`noFloatingAspectCombo`).

## 18. Host folders for favourites and unconfigured sources - DONE
- `fsTagForPost` (`src/remote/remotesource.cpp:1141`) resolves the entry
  by key/label first, then falls back to the post URL host (and the file
  host only for unknown labels): Yandere rows file under `yande.re/`, not
  `Yandere/`. Known mirrors (konachan.net/com, soybooru.com) resolve via
  `entryForHost` (`:932`).
- Verify: `test_remotesource` (`labelAndHostFallbacks`),
  `test_downloadplan` (`yandereUsesHostFolder`).

## 17. Soybooru PoW after a Cloudflare pass - DONE
- The browser fallback now checks its body: when Cloudflare passed but the
  origin still returns `pow_required`, `beginPowRetry`
  (`src/remote/remotesource.cpp:849`) solves the PoW natively on the search
  QNAM and retries once, same as the plain HTTP 449 path.
- Verify: `test_remotesource` challenge-body cases; a live wall cannot be
  unit-tested, the branch is exercised by the shared 449 solver tests.

## 16. `_Stray` is a Booru Explorer feature - DONE
- `StrayDialog` hint and `docs/GLOSSARY.md` state strays are for random
  URLs and paid/exclusive uploads (Patreon etc.), not a LoliSnatcher mode
  (`src/ui/straydialog.cpp:20`).
- Verify: `check_translations` (new source string not yet in locale files,
  same as other recent strings).

## 15. LoliSnatcher import is append-only by default - DONE
- Imports only add rows. `Sync` stays opt-in, now asks for confirmation,
  and `LosnImport::staleFavourites` scopes removals to sites present in
  this backup (`src/library/losnimport.cpp:112`), so a Gelbooru backup
  can never drop stray/Danbooru favourites.
- Verify: `test_losnimport` (`syncOnlyTouchesBackupSites`).

## 14. Favourites origin badges - DONE
- `keyForLabel` (`src/remote/remotesource.cpp:976`) matches configured
  labels/short names case- and punctuation-insensitively; `entryForPost`
  then falls back to the post/file URL host (`entryForHost`), so konachan,
  soybooru and visuabusters favourites keep their favicon/short name.
- Verify: `test_remotesource` (`labelAndHostFallbacks`).

## 13. Favourites tag categorization + counts - DONE
- Favourites run the same cached classifier as browse tabs
  (`maybeClassifyBrowse`, `src/ui/mainwindow_classify.cpp:87`) and persist
  splits; flat legacy LOSN rows offer `tagsGeneral` as untyped once
  (`src/library/favdb.cpp`), healing the 4-vs-200 split.
- `applyTagBuckets` (`src/library/index.cpp:342`) removes a tag from every
  bucket before placing it in the verdict bucket, so one tag cannot be
  bold in two sections with split counts. Sidebar and viewer count each
  post's tag once and show still-unclassified tags under General
  (`src/ui/mainwindow_sidebar.cpp:172`, `src/ui/viewerpage_tags.cpp:40`).
- Verify: `test_index` (`tagLeavesEveryOtherBucket`), `test_favdb`
  (`flatRemoteRowOffersGeneralForClassification`); full `ctest` 38/38.

## 12. Duplicate checking on LoliSnatcher import - DONE
- Favourites are already deduped by `(site, post_id)` (`FavDb::addMany`,
  `INSERT OR IGNORE`); media is skipped when its md5 exists anywhere under
  the download root (`md5PathsOnDisk`).
- New: duplicate rows inside one backup (same md5 or same target path) now
  queue a single fetch instead of writing the same file twice
  (`LosnImport::startDownloads`); `DownloadManager::dedupeJobs` is the
  last-resort guard and records skipped duplicates in the summary
  (`src/remote/downloadmanager.cpp`).
- Verify: `test_downloadplan` (`dedupeJobsKeepsFirstTarget`).

## 11. Download badge follows the filesystem - DONE
- Grid/viewer `↓` no longer trusts `FavDb.local_path`: remote posts resolve
  a content md5 (post/raw sidecar, then file URL) against a download-folder
  scan, or the planned host-folder filename when no hash exists
  (`src/ui/mainwindow_heal.cpp:102`, `mainwindow.h:332`).
- Deleting files (grid/viewer/unfavourite) or finishing a batch invalidates
  the scan; external deletes are caught by the folder mtime.
- Verify: `ctest -R 'losnimport|downloadplan|security|remotesource|stray'`
  (6/6); badge needs GUI eyes, no unit seam.

## 10. LoliSnatcher imports use host folders - DONE
- `LosnImport::targetPaths` now mirrors `DownloadManager::plan`:
  `<host>/<host-without-punctuation>_<md5>.<ext>` (e.g. `gelbooru.com/
  gelboorucom_<md5>.jpg`) instead of a parallel `Gelbooru/` tree
  (`src/library/losnimport.cpp:188`, shared `RemotePosts::safeFolderSegment`
  at `src/remote/remotesource.cpp`).
- Re-imports classify by md5 anywhere under the download folder
  (`md5PathsOnDisk`), refresh sidecars beside the real file, link
  favourites to it, and migrate old app-made `<Label>/` files into the host
  folder once. Already-downloaded media is never re-fetched.
- Verify: `test_losnimport` (`targetPathsUseHostFolderLikeDownloader`,
  `md5PathsOnDiskReturnsRealFile`); 6/6 targeted.

## 9. Downloader folder naming shared with imports - DONE
- `safeSiteDir` moved to `RemotePosts::safeFolderSegment` so the batch
  downloader and LoliSnatcher importer cannot drift on folder sanitizing
  (`src/remote/downloadmanager.cpp:164`, `remotesource.h`).


## 8. Phantom notes/comments + unreadable expanded thread - DONE
- Notes claimed on nearly every post: note/comment payloads are now scoped
  to the requested `post_id` when the row carries one, so engines that
  ignore the post filter (site-wide feed) can no longer paint another
  post's annotations; deleted notes (`is_active=false`) are skipped.
  Presence stamps also treat "0.0"/"null"/"none"/epoch-zero strings as
  absent (`src/engines/booruengine.cpp`).
- Notes never loaded on Gelbooru dapi builds that answer the note table
  as XML despite `json=1` (live safebooru.org): both notes and comments
  parse XML tables too (`src/remote/postmeta.cpp`).
- `Show all N comments` stacked the expanded rows on the collapsed ones:
  `rebuildMetaTail` detached widgets without deleting them (they kept
  painting at their old geometry). Rows now toggle in place; rebuilds
  delete the widgets (`src/ui/viewerpage_meta.cpp`).
- Note click mapped overlay boxes to `m_notes` by index, so skipped notes
  shifted the popup body; the body now rides the graphics item.
- Verify: `test_postmeta` (scoping, XML, inactive) + `test_commentmarks`
  (null-ish stamps) + `test_metathread` (notes bodies + expanded comment
  rows never overlap); 38/38.

## 7. Double-click out of fullscreen un-maximizes - DONE
- Double-click in the viewer emits `fullscreenToggleRequested`, which
  routes through `MainWindow::toggleViewerFullscreen` -> `setViewerFullscreen`,
  already fixed in task 3 (capture state before chrome hides, restore via
  `setWindowState`). No separate code path; the same fix covers it.
- Verify: Xvfb double-click (enter/exit) roundtrip + 37/37.
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

## Fixed in this session
- Removed the orphaned omnibar aspect combo that floated at the top-left as `3:4`; ratio stays in Preferences.
- Origin badges resolve downloaded-favourite rows via post/file URL host fallback (glegle.gallery etc.) and are hidden on single-booru tabs, kept on favourites/downloads/multibooru.
- Sidebar right-click recategorise reloads the pane and grid; emptied categories disappear immediately.
- glegle.gallery post URLs use root-id shape (/{id}); stray parsing updated.
- Origin badge text contrast on light backgrounds.
- Omnibar filters restricted to online browsing; aspect button moved to settings; counts removed from age rating dropdown.
- Sidebar right-click recategorise (category+booru scoped).
