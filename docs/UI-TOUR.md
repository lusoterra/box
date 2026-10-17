# UI tour — box (terms in `docs/GLOSSARY.md`)

Window layout, top to bottom: menubar → tab bar → omnibar → sidebar +
grid → status bar.

## 01-welcome — home tab (`WelcomePage`, `src/ui/welcomepage.cpp`)
Tab 0, shown when no tab is active. From top: app icon + `box` title,
tagline + version, four big buttons — `Back to last tab` (only when a
session was saved), `New browse tab` (opens default boorus), `♥
Favourites`, `⭳ Downloads` — and the `Always load last session on
startup` checkbox (mirrors Preferences → General). Optional mascot image
pins bottom-right behind everything (double-click cycles size; setting
`home/mascot*`).

## 02-browse-grid — booru tab (browse mode, `MainWindow`, `src/ui/mainwindow.cpp`)
One tab per archive/site set. Tab bar button shows favicon + label + ✕.
Left: sidebar tag sections (COPYRIGHT, CHARACTERS, ARTISTS, TAGS, META)
with counts; click toggles filter pills. Center: post grid. Bottom-left:
`40 results · page 1` and infinite scroll (more loads at the bottom).

## 03-omnibar — the control row under the tab bar
Left to right: tag search box (placeholder `Search tags…`, autocomplete
popup, `/` focuses, Enter searches) → rating dropdown (`All`, default
label) → sort dropdown (`Recently added`/`Newest`/`Oldest`/`Top
Rated`/`Low Rated`/`Random` — server-side `order:`/`sort:` tags in browse
mode, client-side locally) → aspect dropdown (`1:1`, grid cell shape) →
`Boorus…` (per-tab source picker, multibooru merge) → `Download (N)`
(batch-downloads the selection) → `Clear` (resets pills/query) → reload
`↺` (`Rescan folder` locally, re-search remotely) → `Page 1 · 40 results`
→ grid-zoom slider (far right).

## Grid card anatomy (see 19-badge-closeup)
Thumbnail with, overlaid: top-right media kind (`▶` video / `GIF`);
top-left purple robot = AI-generated (`isAiPost`); bottom-left origin
badge = site favicon + short name (`DAN`/`SFW`/…); below, info card with
rating chip (`G`), score (`+12`), `#postid`, and the file name.

## 04-file-menu — File
`Open ▸` (folder picker), `Browse Downloads`, `Import LoliSnatcher
backup…`, `Download/Add Stray…`, `Update downloaded sidecars…`, `Quit`.

## 05-tabs-menu — Tabs
`Close Folder`, `♥ Favourites` (database-only tab), `Reload` (`F5` —
rescan locally, re-search remotely).

## 06-edit-menu — Edit
`Preferences…` (`Ctrl+,`, tabbed dialog shots 12–16), `Load Custom
Stylesheet…` (`.qss` overlay).

## 07-help-menu / 08-about — Help
Single item `About box`; the dialog shows version, tagline, repo URL,
copyright, license, Qt version.

## 09-plus-menu — ＋ new-tab picker (`showPlusMenu`)
`Home`, `Favourites`, `Downloads`, then one entry per configured booru
(favicon + label). One click = one tab; merging stays per-tab via
`Boorus…`.

## 10-boorus-menu — Boorus… per-tab picker (`m_sitesMenu`)
Checkable entries: configured sites by label, then virtual sources `♥
Favourites` and `⭳ Downloads` (merged locally, page 1 only). Checking a
set re-searches immediately (`onSitesChanged`).

## 11-viewer — same-window post viewer (`ViewerPage`, `src/ui/viewerpage.cpp`)
Toolbar: `Back`, `‹ 8 / 40 ›` stepping, `Fit`, `100%`, `Open post`
(site page, http(s) only), `Download`, `Tags` (toggle panel), refresh,
`Open file`, `Copy path`. Media area: wheel-zoom/drag-pan stills, GIFs,
looping video with seek/volume. Right: metadata (`#id`, site, URLs,
score, rating with `(inferred)` flag, dimensions, ext) and clickable tag
groups (Artist/Character/Copyright/General/Meta + native custom
categories), then notes + comments on remote posts.

## 12-prefs-general — Preferences → General
`Default tab`, `Language`, `Always load last session on startup`,
`Archive history` (+ `Clear history`), Favourites toggles
(auto-download/auto-favourite/unfavourite-deletes-file), blacklist box.
Bottom: `Defaults` (reset page), `OK` / `Apply` (greys out when clean —
`updateApplyState`) / `Cancel`.

## 13-prefs-library — Preferences → Library
Index cache, thumbnail cache (+ disk), full-image cache, auto tag
classification, `Cache size limit`, `Thumbnail size`, `Clear caches…`.

## 14-prefs-browse — Preferences → Browse
`Page size`, `Default boorus` checklist (new tabs + fallbacks), `Data
folder` / `Download folder` (empty = XDG defaults under `box/`), Booru
manager: entry list + `Name`/`short` (grid badge text)/`Badge color`/`Base
URL`/`Type` (engine) editor with cookie capture + favicon override.

## 15-prefs-tags — Preferences → Tags
`Tag type overrides` (manual `tag → bucket`, wins over fetches),
`Categories per booru type` grid (untick buckets a taxonomy lacks —
folds to general), `Explicit detection` word list (offline
rating upgrades; edits invalidate the scan cache).

## 16-prefs-appearance — Preferences → Appearance
`Theme`, `Accent`, `Custom .qss`, `Home mascot` path + `Mascot opacity`
slider + `Mascot margin` spinbox, `Grid` ratio/scroll, `Video playback`
(mute/loop/volume), `Viewer` max side.

## 17-multibooru — merged tab
Tab title joins sources (`Safebooru + Danbooru + Favourites`); grid mixes
per-site results, each card keeping its own origin badge; one failing
site shows a status-bar error while the rest render.

## 18-favourites — ♥ tab (empty state)
Same chrome over `store.db` rows (no files): star posts to fill it, then
mass-download; non-empty rows render identical cards, dead files relink
by md5.

## 19-badge-closeup — origin badge detail
16px site favicon (auto-fetched to `favicons/auto/`, overridable in the
Booru manager) + short name on the entry's badge color; `G` rating chip,
score, `#id`, file name below.

## Not yet documented (keep unchecked until done)
- [ ] Downloads tab (non-empty: sidecar-backed offline rows)
- [ ] Downloads progress dialog (batch with per-file rows)
- [ ] Download/Add Stray dialog
- [ ] Cookie capture browser dialog
- [ ] Update-sidecars scope dialog (multi-site)
- [ ] Search-errors inspector (status-bar probe popup)
