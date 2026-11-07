# UI tour - box (terms in `docs/GLOSSARY.md`)

Window layout, top to bottom: menubar → tab bar → omnibar → sidebar +
grid → status bar.

## 01-welcome - home tab (`WelcomePage`, `src/ui/welcomepage.cpp`)
Tab 0, shown when no tab is active. From top: app icon + `Box` title,
tagline + version, four big buttons - `Back to last session` (only when
a session was saved: restores every tab, lands on the saved current
one, and returns to its post/scroll; if every tab was closed it warns
"Nothing to resume" instead of resurrecting one), `New browse tab` (restores the session first when empty, then
opens the Default-tab booru or the default boorus), `♥
Favourites`, `⭳ Downloads` - and the `Always load last session on
startup` checkbox (mirrors Preferences → General). Tabs → `Start fresh
session` closes everything and forgets the session. Optional mascot image
pins bottom-right behind everything (shipped default `マニフェスト.jpg`;
double-click cycles size; `home/mascot*`). File > Open lists the last folders (plus a full-history
picker) for mouse-driven reopening.

## 02-browse-grid - booru tab (browse mode, `MainWindow`, `src/ui/mainwindow.cpp`)
One tab per archive/site set. Tab bar button shows favicon + label + ✕.
HyBooru tabs hydrate in the background: summaries arrive tagless, so each
loaded row is detail-fetched through a dedicated queue and the sidebar
fills without opening posts one by one.
Left: sidebar tag sections (ARTISTS red, CHARACTERS green, COPYRIGHT
purple, META orange, TAGS/general white) with counts, each tinted in its
bucket colour like the filter pills, plus
extra sections for native custom categories (Nuuru flags, Szuru
categories) seen on the page - or folded into a classic bucket when
mapped under Preferences → Tags. Category labels read as words
(`ENTITY TYPE`, not `entity_type`); empty categories hide entirely until
tags show up, and tiny ones (≤3 tags) shrink so many fit at once;
click toggles filter pills, right-click stars/un-stars favourite tags. The
omnibar also has a history button (clock icon): executed searches are
replayed from there when `Remember search history` is enabled. A captcha/PoW wall failure pops the solve browser open (opt out in Preferences → Browse, back to a small menu: solve the challenge in the capture browser, or drop the booru (deselect in multibooru, close a solo tab)). Center: post grid. Bottom-left: `40 results · page 1` and infinite scroll (more loads at the bottom); a failed query leaves its one-liner there (click = full server response) plus a `Solve … in browser…` button while walls are reported. The browser
opens AT the failing URL (failed search request, failed file URL) instead of
the homepage, since file/media hosts challenge while `/` lets you through.
API-gated sites (soybooru) answer HTTP 449 `pow_required`/`captcha_required`
JSON instead of HTML: proof-of-work solves itself in-app (same stack, one
retry, clearance saved to the entry); only captchas open the solve browser -
search on the site there so the challenge fires, then save. Repeat auto-popups
within minutes are suppressed (the button stays).

## 03-omnibar - the control row under the tab bar
Left to right: `★` favourite-tags menu (click a star to add it to the
search; coloured by tag bucket, scrolls past ~16 rows; manage/remove in
Preferences → General) → tag search box (placeholder `Search tags…`, autocomplete
popup ranked exact-match first, then starred tags, then most-used, then your
recent tokens - `/` focuses, Enter searches) → rating dropdown (`All`, default label) → media dropdown
(stills / animated GIFs / anything animated / videos / sound, adds a
`filetype:` tag - expanded to native `animated`/`video` tags on Danbooru
where live-verified, `filetype:` elsewhere; typing `animated` /
`animated_gif` / `video` filters by media on Gelbooru-family/Moebooru
too) → sort dropdown (`Recently added`/`Newest`/`Oldest`/`Top
Rated`/`Low Rated`/`Random` - server-side via translated tags where the
engine supports it (options that would only re-sort client-side disable
themselves per tab; `Recently added`/`Newest` never reorder) → aspect dropdown (`1:1`, grid cell shape) →
`Boorus…` (per-tab source picker, multibooru merge) → `AI` (one-click
filter hiding AI-generated posts) → `Download (N)`
(batch-downloads the selection) → `Clear` (resets pills/query) → reload
`↺` (`Rescan folder` locally, re-search remotely) → `Page 1 · 40 results`
→ grid-zoom slider (far right). Below: filter pills - typed tags
(including `rating:` and `filetype:` media kinds), query
groups, rating/source, plus each booru entry's `defaultTags` as removable
cards (× drops the default for that tab and re-searches). The
rating/category dropdowns are shortcuts for typing: a rating choice renders
as a `rating:<value>` pill, a source choice as a plain pill - same look and
× behavior as typed tags (typing a `rating:` tag clears the box to `All`
so the two never stack).

## Grid card anatomy (see 19-badge-closeup)
Thumbnail with, overlaid: top-right media kind (`▶` video / `GIF`);
top-left status boxes, left to right: purple robot = AI-generated
(`isAiPost`), red `♥` = favourited, green `↓` = downloaded,
blue speech bubble = has comments, amber note = has annotations
(counts/stamps where the engine exposes them; viewer-fetched threads
mark the grid too);
bottom-left origin badge = site favicon + short name (`DAN`/`SFW`/…)
- downloaded rows resolve their folder back to the entry, so they
keep the badge; below, info card with rating chip (`G`), score
(`+12`), `#postid`, and the file name. The tooltip and the viewer
link stay on the booru post page after a download (strays keep the
local file). Right-click menu resolves the on-disk file behind downloaded
remote rows too: `⭳ Download` disables once everything selected is on
disk, `Delete from disk…` enables, `Set as mascot` shows, and `Copy post
link` / `Copy media link` / `Copy file path` put the selected rows on the
clipboard (stills, GIFs and videos alike). Dragging cards exports linked
stills/GIFs as a URL (post page or direct media, per Preferences → Browse)
and local/downloaded stills/GIFs as the file itself; videos are excluded
from drag-out on purpose (too heavy for a drop target).
Tabs drag to reorder (welcome stays first).

## 04-file-menu - File
`Open ▸` (folder picker), `Browse Downloads`, `Import LoliSnatcher
backup…` (favourite and downloaded flags are independent: import favs,
download/download-as-favourite/skip downloaded rows, optionally cross
them), `Download/Add Stray…` (paste direct media or booru post links;
see 20-stray-dialog), `Update downloaded sidecars…`,
`Redownload missing files…`, `Quit`. Media downloads tolerate
empty/misleading `Content-Type` headers (Shimmie2 serves real JPEGs with
none): magic bytes decide, wall pages still fail with a named error.

## 05-tabs-menu - Tabs
`Close Folder`, `Start fresh session` (closes every tab, forgets the
saved session, shows home), `♥ Favourites` (database-only tab),
`Reload` (`F5` - rescan locally, re-search remotely). Library archive
roots are read-only: no Delete there, tag refresh is disabled, and a
hard `refuseArchiveWrite` guard refuses any file write outside the
download dir (Syncthing-safe).

## 06-edit-menu - Edit
`Preferences…` (`Ctrl+,`, tabbed dialog shots 12–16), `Load Custom
Stylesheet…` (`.qss` overlay).

## 07-help-menu / 08-about - Help
Single item `About Box`; the dialog shows version, tagline, repo URL,
copyright, license (AGPLv3 or later), Qt version plus third-party credits
(miniz, Lucide icons, FFmpeg via Qt Multimedia).

Grid cards follow the default category order Artists, Character,
Copyright, General, Meta, then native categories from unusual boorus.

## 09-plus-menu - ＋ new-tab picker (`showPlusMenu`)
`Home`, `Favourites`, `Downloads`, then one entry per configured booru
(favicon + label). One click = one tab; merging stays per-tab via
`Boorus…`.

## 10-boorus-menu - Boorus… per-tab picker (`m_sitesMenu`)
Checkable entries: configured sites by label, then virtual sources `♥
Favourites` and `⭳ Downloads` (merged locally, page 1 only). Checking a
set re-searches immediately (`onSitesChanged`).

## 11-viewer - same-window post viewer (`ViewerPage`, `src/ui/viewerpage.cpp`)
Toolbar: `Back`, `‹ 8 / 40 ›` stepping, `Fit`, `100%`, `Variants`
(parent/child family search; shown only when the API exposes variants),
`Open post` (site page, http(s) only), `Image search` (SauceNAO, Yandex, Google
Lens, IQDB, trace.moe - needs a remote file URL), download icon (toggles to
delete once the file is on disk, like the ♥ button), reload-everything (icon:
tags, comments, media), `Notes`
(translation overlays on the image, on by default; left-click a note marker
opens a larger `Annotation note` popup with selectable text instead of a
hover tooltip),
globe (`Open file`), `Copy file URL`, `Comments` (thread popup with cards,
reuses the cached comments/notes). The tag panel below the groups shows
three threads when the engine provides them: NOTES, ARTIST COMMENTARY
(Danbooru `artist_commentary`, translated text preferred) and COMMENTS. A post can be dragged out from the media as well (stills/GIFs at
fit zoom, same URL/file payload as grid drag; videos excluded). Double-click the media for
fullscreen post view (`MainWindow::setViewerFullscreen`: menubar, tab bar,
omnibar, sidebar, pills and status bar hide while the viewer goes immersive;
`ViewerPage::setImmersive`); right-click offers `Fullscreen`, link clippings,
download/delete and favourite. Esc leaves fullscreen first, then the
viewer; F11 toggles it; arrow keys keep stepping posts while fullscreen.
The omnibar and filter pills
hide in post view (grid tools would
misclick); they return with the grid. A failed comments/notes fetch shows
a clickable warning (`click for details`) opening the same diagnostics
dialog as media failures. Below the title: `AI` / `♥` / `↓`
badges mirroring the grid marks. Downloading or (un)favouriting from
the viewer never leaves it; the grid refreshes behind and the viewer
follows it (removed rows resolve to the nearest surviving post,
twin copies collapse so stepping never sticks). Media area: wheel-zoom/drag-pan stills, GIFs,
looping video with seek/volume/rate (playback halts on every post
switch - no background audio, guarded by a wanted-source gate so stale
`LoadedMedia` ticks never restart the previous video from 0:00; a set global proxy, or a failed direct
stream, buffers through the download stack instead - same referer +
cookies + proxy as stills; the red error label only appears for terminal
failures, never while still loading/buffering; clicking the red label (or
the viewer error note, or a failed row in the Downloads dialog) opens the
redacted URL + reason like a search error does; any failure also arms the
status-bar probe, so clicking the `Media failed…` line opens the
redacted URL + reason like a search error does; a wall-marked failure
also triggers one silent clearance refresh from the capture profile,
then retries the post automatically; fingerprint-walled stills/GIFs
additionally retry through the browser backend, which re-runs page JS
where QNAM replays a dead cookie). Right: metadata (`#id`, site -
downloaded rows show their booru `Label (Engine)` form -, URLs (the
post page, not the downloaded file, except strays),
score, rating with `(inferred)` flag, dimensions, ext) and clickable tag
groups (Artist/Character/Copyright/General/Meta + native custom
categories; right-click a tag for `Add to search` or `Search in
new tab` per booru), then notes + comments on remote posts (long
threads collapse behind a show-all toggle).

## 12-prefs-general - Preferences → General
`Default tab`, `Language`, `Always load last session on startup`,
`Archive history` (+ `Clear history`), Favourites toggles
(auto-download/auto-favourite/unfavourite-deletes-file), blacklist box.
Bottom: `Defaults` (reset page), `OK` / `Apply` (greys out when clean -
`updateApplyState`) / `Cancel`.

## 13-prefs-library - Preferences → Library
Scan index cache, `Media cache` radios (`Cache everything` /
`Cache only SFW pictures/videos` / `Cache nothing (session only)`),
`Also cache full-quality images…`, auto tag classification,
`Cache size limit`, `Thumbnail size`, `Delete cached media…` (greyed
when caching everything; SFW deletes NSFW/unknown entries, Nothing
removes all media) and `Clear scan index…`. Under SFW-only, media you
explicitly downloaded stays cacheable even when NSFW; favouriting alone
does not lift the filter. Blacklisted tags never cache in SFW-only mode,
and an empty/unknown rating is treated as NSFW unless a positive
`rating:g`/`rating:safe`-style tag proves otherwise.

## 14-prefs-browse - Preferences → Browse
`Page size`, `Drag posts as` (what a post drag sends to Discord/other
apps: post page or direct media; per-booru overrides live in the manager),
`Remember search history` (off by default; stored in `store.db`) +
`Clear search history`, `Default boorus` checklist (new tabs + fallbacks), `Data
folder` / `Download folder` (empty = XDG defaults under `box/`),
`Ugoira downloads` (animated webp / animated jxl / keep zip; needs
ffmpeg in PATH, +cjxl for jxl) + `Ugoira frame delay` (fallback when the
API omits pixiv frame timing), `Global proxy` (all traffic; `socks5h://` routes hostnames through
the proxy like curl, per-booru proxy overrides it, `direct` bypasses;
the embedded browser - capture, PoW walls, onion sites - reads it at
startup, so restart after changing it),
Booru
manager: entry list with `↑`/`↓` reorder + `Name`/`Enabled` (off keeps
the config but hides the booru from every picker)/`short` (grid badge text)/`Badge color`
(known hosts get house colors: gelbooru blue, rule34 green, aibooru green,
danbooru/safebooru tan, shimmie gray; editable anytime)/`Base
URL`/`Type` (engine) editor with cookie capture + favicon override.
Per-entry `Tags` (skip auto-categorization), `Drag posts as` (direct
media vs post page; `Use global` follows Preferences → Browse), `Blacklist` (filter
blacklist locally instead of server `-tags`, for tag-capped accounts),
`Default tags`, `Media cache` (`Global` / `All` / `SFW only` / `Nothing`
for this booru, narrowed by the global mode), `Proxy`.

## 15-prefs-tags - Preferences → Taxonomies
`Tag type overrides` (manual `tag → bucket`, wins over fetches),
`Restore presets` asks three times before re-adding shipped boorus (the
list is easy to lose),
`Custom category → bucket` (fold a native category into a classic bucket),
`Custom tag categories` (create named groups and assign `tag → group`;
assigned tags leave Artist/Character/Copyright/Meta/Tags),
`Merge tag categories` (per-booru rows `source1, source2 → Target`:
group native categories under one label or rename a classic bucket, e.g.
`character, variant → Characters` or `* → soybooru_specific`),
`Explicit detection` word list (offline rating upgrades; edits
invalidate the scan cache).

## 16-prefs-appearance - Preferences → Appearance
`Theme` (file-based; Box dark, KeePassXC dark/light, Light, Nord,
Catppuccin Latte/Frappé/Macchiato/Mocha, Dracula, Atom One Dark, Tokyo
Night, System native), `Accent`, `Custom .qss`, `Sidebar: one scrollbar for all
tag categories` (default off), `Show local folder browser in the quick +
menu` (default off), `Home mascot` Browse/Remove/Reset-to-default +
opacity slider + margin, `Grid` ratio/scroll, `Snap grid post size to fill the window` (on by
default; off keeps the exact zoom-slider width and its right gap),
`Staggered grid rows (brick layout)` (off by default; odd columns sit half
a card lower), `Video playback`
(mute/loop/volume), `Viewer` max side. Neighbour preload is always on:
the two posts on each side warm their viewer URL (sample-first; videos
get a 2 MB Range warm-up, never a cached stream).

## 17-multibooru - merged tab
Tab title joins sources (`Safebooru + Danbooru + Favourites`); grid mixes
per-site results, each card keeping its own origin badge; one failing
site shows a status-bar error while the rest render.

## 18-favourites - ♥ tabs (empty state)
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

## 19-badge-closeup - origin badge detail
16px site favicon (auto-fetched to `favicons/auto/`, overridable in the
Booru manager) + short name on the entry's badge color; `G` rating chip,
score, `#id`, file name below (synthesized `id - host - tags.ext` when the file URL has no name, e.g. Nuuru `/file`; downloads keep `site_md5` names).

## 20-stray-dialog - Download/Add Stray (`StrayDialog`)
Big link box (one URL per line) plus `Auto-favourite downloaded/imported
strays`, and four buttons: `Download` (fetches media into `_Stray/` and,
for recognised booru post pages, also resolves the post so its JSON
sidecar carries the real tags - no booru entry is added), `Favourite
only` (same resolution, but only a favourites row: no media download;
the file URL is kept so the card/viewer can stream and a later download
can link it), `Import files…` (local copy-in, hashed names), `Close`.
Direct image/video links pass straight to the downloader. Recognised
post-page URLs (Danbooru/HyBooru `/posts/<id>`, Moebooru
`/post/show/<id>`, Shimmie/Nuuru `/post/view/<id>`, Szurubooru
`/post/<id>`, Gelbooru/Safebooru
`index.php?page=post&s=view&id=`) probe their public API first; when the
API refuses anonymous calls (Gelbooru 401) the page itself is read
(`og:image` + typed tag list). Unreadable links are reported once,
without queues of wall pages.
Grid right-click `Edit tags…` on strays: one stray opens the one-box
editor (flat, auto-categorized on save); two or more (`Edit tags…
(N strays)`) open `Mass edit stray tags (N)`: file list, per-bucket boxes
(Artist / Character / Copyright / General / Meta in that order), `Paste
tags:` (`Add pasted` appends tokens to General), `Copy from URL:` +
`Fetch` steals buckets from a booru post link (same resolver as above,
merged into the current file), `Copy to all` duplicates the current
file to every listed stray, `Save all` writes `tags` + `tags_*` sidecars
(category order kept). Status bar reports `Saved tags for N stray(s)`.

## Not yet documented (keep unchecked until done)
- [ ] Downloads tab (non-empty: sidecar-backed offline rows)
- [ ] Downloads progress dialog (batch with per-file rows)
- [ ] Cookie capture browser dialog
- [ ] Update-sidecars scope dialog (multi-site)
- [ ] Search-errors inspector (status-bar probe popup)

## 19-theme-files - adding a theme
Themes are data files, like translations: copy `themes/<key>.ini` to a
new `themes/<your-key>.ini` in the repo (reconfigure/build) or drop it
into `~/.config/box/themes/` for runtime discovery. `[theme]` keys:
`name`, `order`, `dark`, `window`, `base`, `alternate`, `button`, `text`,
`dim`, `disabled`, `border`, `accent`. The Preferences combo rebuilds
from those files; a user file with the same name overrides the bundled one.
