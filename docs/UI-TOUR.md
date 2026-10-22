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
`home/mascot*`). File > Open lists the last folders (plus a full-history
picker) for mouse-driven reopening.

## 02-browse-grid — booru tab (browse mode, `MainWindow`, `src/ui/mainwindow.cpp`)
One tab per archive/site set. Tab bar button shows favicon + label + ✕.
Left: sidebar tag sections (COPYRIGHT, CHARACTERS, ARTISTS, TAGS, META)
with counts, each tinted in its bucket colour like the filter pills, plus
extra sections for native custom categories (Nuuru flags, Szuru
categories) seen on the page — or folded into a classic bucket when
mapped under Preferences → Tags;
click toggles filter pills, right-click stars/un-stars favourite tags. A captcha/PoW wall failure pops the solve browser open (opt out in Preferences → Browse, back to a small menu: solve the challenge in the capture browser, or drop the booru (deselect in multibooru, close a solo tab)). Center: post grid. Bottom-left: `40 results · page 1` and infinite scroll (more loads at the bottom); a failed query leaves its one-liner there (click = full server response) plus a `Solve … in browser…` button while walls are reported. The browser
opens AT the failing URL (failed search request, failed file URL) instead of
the homepage, since file/media hosts challenge while `/` lets you through.
API-gated sites (soybooru) answer HTTP 449 `pow_required`/`captcha_required`
JSON instead of HTML: proof-of-work solves itself in-app (same stack, one
retry, clearance saved to the entry); only captchas open the solve browser —
search on the site there so the challenge fires, then save. Repeat auto-popups
within minutes are suppressed (the button stays).

## 03-omnibar — the control row under the tab bar
Left to right: `★` favourite-tags menu (click a star to add it to the
search) → tag search box (placeholder `Search tags…`, autocomplete
popup ranked exact-match first, then starred tags, then most-used, then your
recent tokens — `/` focuses, Enter searches) → rating dropdown (`All`, default label) → media dropdown
(stills / animated GIFs / anything animated / videos / sound, adds a
`filetype:` tag) → sort dropdown (`Recently added`/`Newest`/`Oldest`/`Top
Rated`/`Low Rated`/`Random` — server-side via translated tags where the
engine supports it (options that would only re-sort client-side disable
themselves per tab; `Recently added`/`Newest` never reorder) → aspect dropdown (`1:1`, grid cell shape) →
`Boorus…` (per-tab source picker, multibooru merge) → `AI` (one-click
filter hiding AI-generated posts) → `Download (N)`
(batch-downloads the selection) → `Clear` (resets pills/query) → reload
`↺` (`Rescan folder` locally, re-search remotely) → `Page 1 · 40 results`
→ grid-zoom slider (far right). Below: filter pills — typed tags
(including `rating:` and `filetype:` media kinds), query
groups, rating/source, plus each booru entry's `defaultTags` as removable
cards (× drops the default for that tab and re-searches). The
rating/category dropdowns are shortcuts for typing: a rating choice renders
as a `rating:<value>` pill, a source choice as a plain pill — same look and
× behavior as typed tags (typing a `rating:` tag clears the box to `All`
so the two never stack).

## Grid card anatomy (see 19-badge-closeup)
Thumbnail with, overlaid: top-right media kind (`▶` video / `GIF`);
top-left status boxes, left to right: purple robot = AI-generated
(`isAiPost`), red `♥` = favourited, green `↓` = downloaded;
bottom-left origin badge = site favicon + short name (`DAN`/`SFW`/…)
— downloaded rows resolve their folder back to the entry, so they
keep the badge; below, info card with rating chip (`G`), score
(`+12`), `#postid`, and the file name. The tooltip and the viewer
link stay on the booru post page after a download (strays keep the
local file). Tabs drag to reorder (welcome stays first).

## 04-file-menu — File
`Open ▸` (folder picker), `Browse Downloads`, `Import LoliSnatcher
backup…`, `Download/Add Stray…`, `Update downloaded sidecars…`,
`Redownload missing files…`, `Quit`.

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
(site page, http(s) only), `Image search` (SauceNAO, Yandex, Google
Lens, IQDB, trace.moe — needs a remote file URL), download icon (toggles to
delete once the file is on disk, like the ♥ button), reload-everything (icon:
tags, comments, media), `Notes`
(translation overlays on the image, on by default),
globe (`Open file`), `Copy file URL`, `Comments` (thread popup with cards,
reuses the fetched notes/comments cache). The omnibar hides in post view
(grid tools would
misclick); it returns with the grid. Below the title: `AI` / `♥` / `↓`
badges mirroring the grid marks. Downloading or (un)favouriting from
the viewer never leaves it; the grid refreshes behind and the viewer
follows it (removed rows resolve to the nearest surviving post,
twin copies collapse so stepping never sticks). Media area: wheel-zoom/drag-pan stills, GIFs,
looping video with seek/volume/rate (a set global proxy, or a failed direct
stream, buffers through the download stack instead — same referer +
cookies + proxy as stills; the red error label only appears for terminal
failures, never while still loading/buffering). Right: metadata (`#id`, site —
downloaded rows show their booru `Label (Engine)` form —, URLs (the
post page, not the downloaded file, except strays),
score, rating with `(inferred)` flag, dimensions, ext) and clickable tag
groups (Artist/Character/Copyright/General/Meta + native custom
categories; right-click a tag for `Add to search` or `Search in
new tab` per booru), then notes + comments on remote posts (long
threads collapse behind a show-all toggle).

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
folder` / `Download folder` (empty = XDG defaults under `box/`),
`Ugoira downloads` (animated webp / animated jxl / keep zip; needs
ffmpeg in PATH, +cjxl for jxl) + `Ugoira frame delay` (fallback when the
API omits pixiv frame timing), `Global proxy` (all traffic; `socks5h://` routes hostnames through
the proxy like curl, per-booru proxy overrides it, `direct` bypasses;
the embedded browser — capture, PoW walls, onion sites — reads it at
startup, so restart after changing it),
Booru
manager: entry list + `Name`/`short` (grid badge text)/`Badge color`
(known hosts get house colors: gelbooru blue, rule34 green, aibooru green,
danbooru/safebooru tan, shimmie gray; editable anytime)/`Base
URL`/`Type` (engine) editor with cookie capture + favicon override.

## 15-prefs-tags — Preferences → Tags
`Tag type overrides` (manual `tag → bucket`, wins over fetches),
`Custom category → bucket` (fold a native category into a classic bucket),
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

## 18-favourites — ♥ tabs (empty state)
Every `Favourites` action opens a fresh tab; keep several with different
tags/filters side by side (titles show their filters). Same chrome over
`store.db` rows (no files): star posts to fill it, then
mass-download; non-empty rows render identical cards, dead files relink
by md5. `Delete from disk…` also removes kept favourites left with no
renderable source (no file, no remote media/post link) instead of leaving
empty cards; post-linked rows stay (viewer refresh can heal them). Files
removed outside the app hide from the grid the same way (never marked
downloaded) until they return. File -> `Redownload missing files…`
re-pulls missing favourites with a download source on demand;
sourceless ones just stay hidden. GUI deletes
also forget the download link, so the action only ever covers files
deleted behind the app's back.

## 19-badge-closeup — origin badge detail
16px site favicon (auto-fetched to `favicons/auto/`, overridable in the
Booru manager) + short name on the entry's badge color; `G` rating chip,
score, `#id`, file name below (synthesized `id - host - tags.ext` when the file URL has no name, e.g. Nuuru `/file`; downloads keep `site_md5` names).

## Not yet documented (keep unchecked until done)
- [ ] Downloads tab (non-empty: sidecar-backed offline rows)
- [ ] Downloads progress dialog (batch with per-file rows)
- [ ] Download/Add Stray dialog
- [ ] Cookie capture browser dialog
- [ ] Update-sidecars scope dialog (multi-site)
- [ ] Search-errors inspector (status-bar probe popup)
