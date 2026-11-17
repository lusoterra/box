# Pending tasks (from user goal)

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
- glegle.gallery post URLs use root-id shape (/{id}); stray parsing updated.
- Origin badge text contrast on light backgrounds.
- Omnibar filters restricted to online browsing; aspect button moved to settings; counts removed from age rating dropdown.
- Sidebar right-click recategorise (category+booru scoped).
