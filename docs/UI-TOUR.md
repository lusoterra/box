# UI tour - box (terms in `docs/GLOSSARY.md`)

Window layout, top to bottom: menubar → tab bar → omnibar → sidebar +
grid → status bar. The status bar is a darker informational strip in
every theme (`Theme::statusBarBackground/statusBarText`, native too),
with contrast-checked text, so it reads as chrome rather than page
content even where the mascot passes behind it.

## 00-startup - launch + one-time migrations
First paint is the home tab (`01-welcome` below). Before the main window
opens its databases, the app runs pending one-time upgrades
(`src/migrations/`, `docs/MIGRATIONS.md`): legacy settings keys, the
booru registry, the favourites schema/identities, cache tracking and
on-disk layout moves. When work is pending a small modal dialog -
`Running migrations…` (or `Migrating vX → vY…` across schema
generations) with a progress bar and the current step - covers startup;
with nothing to do there is no dialog and boot is unchanged. A missing
configured storage folder still warns first (Use defaults / Change
folders / Quit), and a moved data folder re-runs the upgrades for the
new location on Apply.

## 01-welcome - home tab (`WelcomePage`, `src/ui/welcomepage.cpp`)
Tab 0, shown when no tab is active. From top: app icon + `Box` title,
tagline + version, four big buttons - `Back to last session` (only when
a session was saved: restores every tab, lands on the saved current
one, and returns to its post/scroll; if every tab was closed it warns
"Nothing to resume" instead of resurrecting one), `New browse tab` (restores the session first when empty, then
opens the Default-tab booru or the default boorus), `Favourites`
(filled heart icon), `Downloads` (download icon) - and the `Always load
last session on
startup` checkbox (mirrors Preferences → General). Tabs → `Start fresh
session` closes everything and forgets the session. Optional mascot picture
pins bottom-right behind everything (shipped default `マニフェスト.jpg`;
still or animated GIF/WebP, never videos; double-click cycles size;
`home/mascot*`). Leaving the home tab idle for
180 s shrinks the mascot to the small preset and makes it bounce around
the page like a DVD logo; shift+left click starts it immediately (easter
egg/testing), and any click, key, wheel or window resize snaps it back
to the corner at the chosen size (moving the window does not count).
File > Open lists the last folders (plus a full-history picker) for
mouse-driven reopening.

## 02-browse-grid - booru tab (browse mode, `MainWindow`, `src/ui/mainwindow.cpp`)
One tab per archive/site set. Tab bar button shows favicon + label + ✕.
Clicking a tab switches at once: the tab bar, title and toolbar follow the
click immediately, and the grid shows a centred busy bar with `Loading
<tab>…` until that tab's rows are ready (DB decode, archive scan or site
search); the veil drops as soon as the cards land.
HyBooru tabs hydrate in the background: summaries arrive tagless, so each
loaded row is detail-fetched through a dedicated queue and the sidebar
fills without opening posts one by one.
Left: sidebar tag sections (ARTISTS red, CHARACTERS green, COPYRIGHT
purple, META orange, TAGS/general white) with counts, each tinted in its
bucket colour like the filter pills, plus
extra sections for native custom categories (Nuuru flags, Szuru
categories) seen on the page - or folded into a classic bucket when
mapped under Preferences → Tags. Category labels read as words
(`ENTITY TYPE`, not `entity_type`); empty or single-tag categories hide
entirely until they hold two tags (a single-post page, e.g. an `id:`
search, keeps its one artist/character/copyright tag visible), and tiny
ones (2-3 tags) shrink so many
fit at once;
click toggles filter pills, right-click stars/un-stars favourite tags or
recategorises one (the pane reloads in place, so a source category left
with its last tag moved away disappears immediately). The
omnibar has no extra buttons: typing (or clicking it empty) drives the
suggestions. A captcha/PoW wall failure pops the solve browser open (opt out in Preferences → Browse, back to a small menu: solve the challenge in the capture browser, or drop the booru (deselect in multibooru, close a solo tab)). Grid thumbnails hit the same walls: a failed preview retries through the capture browser on its own, and the first failure per site opens one capture pass (never one browser per black card), including on Favourites tabs. Center: post grid. Bottom-left: `40 results · page 1` and infinite scroll (more loads at the bottom); a failed query leaves its one-liner there (click = full server response) plus a `Solve … in browser…` button while walls are reported. The browser
opens AT the failing search request. Media walls on a sibling CDN
(`cdn.donmai.us`) open the site root instead: that CDN page can serve the
image with no challenge (and mint no cookies), while one site clearance
unlocks its media host. After a successful capture the visible grid
thumbnails and the open viewer retry immediately.
Grid keyboard: `Enter` opens the focused card only when exactly one is
selected, `Space` selects/deselects it, arrows move through the grid,
`Ctrl+D` downloads the selection (or the focused card), `/` or `Ctrl+L`
focuses the tag search, `Ctrl+O` opens the post page and `Ctrl+Shift+O`
opens the media in the default viewer. `Esc` clears filters; `F5`/`Ctrl+R`
reload.
API-gated sites (soybooru) answer HTTP 449 `pow_required`/`captcha_required`
JSON instead of HTML: proof-of-work solves itself in-app (same stack, one
retry, clearance saved to the entry); only captchas open the solve browser -
search on the site there so the challenge fires, then save. Repeat auto-popups
within minutes are suppressed (the button stays).

## 03-omnibar - the control row under the tab bar
Left to right: tag search box (placeholder `Search tags…`, `/` focuses)
→ rating dropdown (`All`/`General`/`Sensitive`/`Questionable`/`Explicit`; shown on browse, favourites and both downloads views - server-side on remote tabs, client-side on local/favourites) → source dropdown (`All sources` + one row per booru/folder with counts; shown on Favourites and both downloads views, filters the grid client-side and becomes a removable pill) → media dropdown
(`All media` / `Images` / `GIFs` / `Anything animated` / `Videos`, shown only on
tabs backed by the download folder (`Downloads` source or `File → Browse Downloads`);
filters files under the download folder by extension - a `GIFs` choice covers gif/animated webp/jxl, `Anything animated`
adds mp4/webm; no tags or MIME sniffing) → sort dropdown (`Recently added`/`Newest`/`Oldest`/`Top
Rated`/`Low Rated`/`Random`; shown on browse, favourites and downloads - server-side via translated tags where the
engine supports it on remote tabs, all orders client-side on local tabs and favourites; remote options that would only re-sort client-side disable
themselves per tab, `Recently added`/`Newest` never reorder on remote).
`Recently added` is the default on new tabs: favourites sort by the DB's
added time, local tabs/Downloads by file arrival time (mtime), and
remote browsing keeps the API's native newest-first order. →
`Boorus…` (per-tab source picker, multibooru merge; checkable entries
toggle in place and keep the menu open) → `AI` (one-click
filter hiding AI-generated posts) → `Download (N)`
(batch-downloads the selection) → `Clear` (resets pills/query) → reload
reload button (circular-arrows icon; `Rescan folder` locally,
re-search remotely) → `Page 1 · 40 results`
→ grid-zoom slider (far right).

Omnibar suggestions: clicking (or tab-focusing) the empty box opens the
favourite-tag dropdown - scrollable, coloured by bucket, most-used first.
Every left click appends that tag to the box and leaves the dropdown open,
so a whole query can be picked with the mouse; Esc closes the dropdown
only (the second Esc clears pills/query as before). Typing ranks exact
match, then starred tags, then cached/archive tags by post count, then
remote answers that merge in when they land; every row shows its post
count right-aligned (`500`, `7k`, `1.5M`). Cached counts a host has not
re-confirmed in a week are re-fetched instead of displayed, so the order
never relies on stale popularity. Multibooru tabs rank through their
first booru (last one when the first has no URL). Enter commits: the box is
wiped and each tag becomes a pill; a tag already worn as a pill is
ignored, and `-tag` replaces the existing `tag` pill (same for the other
direction), so `-tag other` flips `tag` and adds `other` in one go.
Up/Down + Enter commits exactly the highlighted suggestion (never the
partially typed token as a second pill); the box is always wiped.

Below: filter pills - typed tags
(including `rating:` and `filetype:` media kinds), query
groups, rating/source, plus each booru entry's `defaultTags` as removable
cards (the trailing x icon drops the default for that tab and re-searches).
The rating/category dropdowns are shortcuts for typing: a rating choice
renders as a `rating:<value>` pill, a source choice as a plain pill - same
look and x behavior as typed tags (typing a `rating:` tag clears the box
to `All` so the two never stack). The plain-text remainder pill carries a
search icon. Every omnibar dropdown carries a hover tooltip
naming what it filters or sorts (rating, source, downloads media type,
sort order) before you open it.

UI art is bundled Lucide SVG (`assets/icons/`, `src/ui/uicons.h`), never a
font glyph: a missing font on another platform must not turn a mark into a
tofu box or a differently sized symbol.

## Grid card anatomy (see 19-badge-closeup)
Thumbnail with, overlaid: top-right media kind (play icon = video, `GIF`
text = animation); top-left status boxes, left to right: purple robot =
AI-generated (`isAiPost`), red heart icon = favourited, green download
icon = the media file is
actually present under the download folder (filesystem scan by md5, not
a stale favourites DB link),
blue speech bubble = has comments, amber note = has annotations
(counts/stamps where the engine exposes them; viewer-fetched threads
mark the grid too);
while the preview is still loading, a muted spinner sits in the middle
of the empty card (queued behind other thumbnails) and turns into a thin
muted progress bar along the card's bottom edge once bytes are moving;
bottom-left origin badge = site favicon + short name (`DAN`/`SFW`/…)
- downloaded rows resolve their folder back to the entry, so they
keep the badge; single-booru tabs omit it (the tab already names the
source), while favourites, downloads and multibooru tabs always show it;
below, info card with rating chip (`G`), score
(`+12`), `#postid`, and the file name. The file line elides in the
middle, so hovering just that line shows the untruncated name in a
tooltip. The viewer link stays on the booru post page after a download
(strays keep the local file). Right-click menu resolves the on-disk file behind downloaded
remote rows too: `Download` disables once everything selected is on
disk, `Delete from disk…` enables, `Set as mascot` shows for pictures
only (never videos, still or animated), and `Copy post
link` / `Copy media link` / `Copy file path` / `Copy file (clipboard)`
put the selected rows on the clipboard (stills, GIFs and videos alike).
A single selected row also has `View metadata…` (summary + tags + raw
JSON tree), `Hydrus tags…` when Preferences → Hydrus is enabled and a
local file or booru md5 is available (read-only lookup by content hash),
and `Show in folder` (file manager opens with the file
selected). Multi-row `Unfavourite` and any multi-post `Download` ask for
confirmation first (count + destination; unfavourite also warns when
the settings delete files); `Delete from disk…` always confirms.
Favourite-only remote cards keep `Copy file (clipboard)`: missing bytes
are fetched into the shared temp folder (`/tmp/box-ext` on Linux) before
the clipboard gets the file URL. Dragging cards exports linked
stills/GIFs as a URL (post page or direct media, per Preferences → Browse)
and local/downloaded stills/GIFs as the file itself; videos are excluded
from drag-out on purpose (too heavy for a drop target).
Tabs drag to reorder (welcome stays first). Tab widths reserve the
selected bold-font difference (plus a couple of pixels), so titles are
not clipped when the tab becomes current; longer titles still marquee.

## 04-file-menu - File
`Open ▸` (folder picker), `Browse Downloads`, `Import LoliSnatcher
backup…` (favourite and downloaded flags are independent: import favs,
download/download-as-favourite/skip downloaded rows, optionally cross
them; imports only add favourites, and the opt-in `Sync` checkbox asks
for confirmation before removing anything and never touches sites absent
from the backup; files land in the same host folders as the regular
downloader, e.g. `gelbooru.com/` not `Gelbooru/`, and already-downloaded
media is matched by md5 anywhere under the download folder; a host folder
appears only when bytes actually arrive, so a walled/offline batch leaves
no empty folder),
`Export library CSV…` / `Import library CSV…` (portable spreadsheet
backup: one row per favourite and per file under the download folder,
with `post_url,file_url,downloaded,favourited,date_added,host,post_id,md5,`
`rating,ext,width,height,tags,local_file`; `host` is the site hostname
(`gelbooru.com`, `yande.re`), `post_id` only the real booru id (never the
internal identity hash), and missing `width`/`height` are filled by
probing files that are already on disk - header-only, no network, videos
left blank. Reordered/renamed/older `site` columns, CRLF/BOM and
`true/false` or `1/0` are accepted. Import opens the same options dialog
as the LoliSnatcher import and keeps `date_added` order),
`Download/Add Stray…` (paste direct media or booru post links;
see 20-stray-dialog), `Share library on LAN…` (read-only Danbooru API
for LoliSnatcher or another Box; see 22-lan-sharing),
`Update downloaded sidecars…`, `Redownload missing files…`, `Quit`.
Media downloads tolerate
empty/misleading `Content-Type` headers (Shimmie2 serves real JPEGs with
none): magic bytes decide, wall pages still fail with a named error.
Closing while a download runs asks `Abort and quit` (delete partials),
`Finish current, then quit`, `Finish all, then quit`, or
`Keep downloading`; a later launch offers Continue / Remove history /
Close Box for the unfinished queue. Pressing Download on a post the
viewer already buffered moves those full-res bytes into the folder
instantly, and waiting for a mid-flight viewer fetch pins it so
navigation cannot abort it.

## 05-tabs-menu - Tabs
`Close Folder`, `Start fresh session` (closes every tab, forgets the
saved session, shows home), `Favourites` (database-only tab, heart icon),
`Reload` (`F5` / `Ctrl+R` - rescan locally, re-search remotely). Tab keys:
`Ctrl+T` opens a new default tab (Default tab booru, else the default
boorus), `Ctrl+W` closes the current tab, `Ctrl+Shift+T` reopens closed
tabs newest-first (a 20-tab LIFO stack; the tab's kind, filters and open
post return, content rescans/re-searches). Switching a tab paints the
grid veil and swaps the left sidebar for a `Loading tags…` placeholder
immediately; the real sections fill in when the deferred scan/search
rebuild lands (never the previous tab's tags). The filter-pill row is
hidden during that window too, so the outgoing tab's tags never flash
over the veil. A tab left on a post reopens that post in the viewer
(identity match, so refills/reorders don't matter) - including
Favourites and Downloads tabs; only asynchronous first loads stay on the
grid until their rows arrive. Library archive
roots are read-only: no Delete there, tag refresh is disabled, and a
hard `refuseArchiveWrite` guard refuses any file write outside the
download dir (Syncthing-safe).

## 06-edit-menu - Edit
`Preferences…` (`Ctrl+,`, tabbed dialog shots 12–16), `Load Custom
Stylesheet…` (`.qss` overlay).

## 06b-tools-menu - Tools
`Find duplicate media…` (`Ctrl+Shift+D`) opens the duplicate review
dialog (section 23).

## 07-info-menu / 08-about - Info
`Statistics…` (popup report: favourited/downloaded post counts and disk
usage, cache size split into thumbnails/full images/scan indexes, cached
tag-vocabulary rows, the most common tags plus top artists, characters and
series, a booru ranking by post count, rating breakdown and - when a
plain local archive tab is open - that archive's post count and top tags;
the DB side renders immediately and the download/cache walk fills in a
moment later), `About Box` (version, tagline, repo URL, copyright,
license AGPLv3 or
later, contact email `lev@lusosterone.net`, PGP fingerprint
`13EA3CFB9ABC093823509BD668C62637ED987692`, Qt version plus third-party
credits: miniz, Lucide icons, FFmpeg via Qt Multimedia), `Keyboard
shortcuts…` (`F1`; multi-line grid/post-view/tabs/window cheatsheet with a
`Documentation…` button that opens the repository), `Documentation…`
(opens the repository).

Grid cards follow the default category order Artists, Character,
Copyright, General, Meta, then native categories from unusual boorus.

## 09-plus-menu - ＋ new-tab picker (`showPlusMenu`)
`Home`, `Favourites`, `Downloads`, `Hydrus` (only when Preferences →
Hydrus is enabled), then `Open view-only folder…` (always: the read-only
gallery-dl/LOSN archive browser; the optional `Open local folder…` entry
follows when enabled in Preferences → General), then one entry per
configured booru (favicon + label). One click = one tab; merging stays
per-tab via `Boorus…`.

## 10-boorus-menu - Boorus… per-tab picker (`m_sitesMenu`)
Checkable entries: configured sites by label, then virtual sources
`Favourites` and `Downloads` (merged locally, page 1 only). Checking a
set re-searches immediately (`onSitesChanged`). The popup stays open
while entries are clicked or toggled with `Space` (`MultiPickMenu`,
`src/ui/multipickmenu.h`), so several sources can be picked without
reopening it; `Enter`, a click outside or a plain (non-checkable) entry
still closes it.

## 11-viewer - same-window post viewer (`ViewerPage`, `src/ui/viewerpage.cpp`)
Toolbar: `Back`, `‹ 8 / 40 ›` stepping, `Fit`, `100%`, `Variants`
(parent/child family search; opens the family in a NEW tab so the
original post and its tab stay browsable; shown only when the API exposes
variants),
`Open post` (site page, http(s) only), `Metadata` (sidecar/API record for
the current post, same as the grid's `View metadata…`), `Image search` (SauceNAO, Yandex, Google
Lens, IQDB, trace.moe - needs a remote file URL), download icon (toggles to
delete once the file is on disk, like the heart button), reload-everything (icon:
tags, comments, media), `Notes`
(translation overlays on the image, on by default; left-click a note marker
opens a larger `Annotation note` popup with selectable text instead of a
hover tooltip; the box outline is the only affordance - no corner dot),
globe (`Open file`), `Copy file URL`, `Comments` (thread popup with cards,
reuses the cached comment rows). Under TAGS a `Loading notes and
comments…` strip with an indeterminate bar appears while the per-post
fetch runs (slow hosts otherwise looked dead); it clears on completion.
Downloaded files paint their notes straight from the sidecar
(`box_notes`) with no notes request at all; the comment thread is still
fetched online on every open. Right-click the media → `Refresh notes`
forces a fresh online fetch and rewrites the sidecar (archive roots are
read-only and are never written).
The panel scrolls vertically only: long tags wrap onto their own line and
wide metadata clips rather than spawning a horizontal scrollbar.
The tag panel below the groups shows
three threads when the engine provides them, in this order: NOTES
(translation bodies, box coordinates on the tooltip; deleted annotations
are skipped; loaded from the engine's note table, or the public post page
on Gelbooru/Safebooru where dapi notes answer empty, or the post detail
object on HyBooru where rect values are image percentages, or the same
detail object on Szurubooru where polygons are canvas fractions), COMMENTS (first
five rows, `Show all N comments` expands
them in place), and ARTIST COMMENTARY at the bottom of the tail. The
commentary section is a compact clickable preview (message text with
markup stripped, cap ~280 chars); clicking it opens the large `Artist
commentary` popup with selectable rich text and clickable links. Sources:
Danbooru `artist_commentary` (translation preferred), Philomena
`description` on Philomena/Booru-on-Rails hosts, HyBooru rect-null Hydrus
notes.
DText/BBCode (Danbooru) and Markdown (Philomena) are rendered
(`[b]`/`[i]`/`[url=]`, `**bold**`, `*italic*`, `[label](url)`, quotes,
bullets); bare URLs auto-link and site-relative links resolve against the
entry. Comments use the same per-engine formatter; notes stay escaped
plain text. A post can be dragged out from the media as well (stills/GIFs at
fit zoom, same URL/file payload as grid drag; videos excluded). Double-click the media for
fullscreen post view (`MainWindow::setViewerFullscreen`: menubar, tab bar,
omnibar, sidebar, pills and status bar hide while the viewer goes immersive;
`ViewerPage::setImmersive`); right-click offers `Fullscreen`, link
clippings, `Copy file (clipboard)` / `Open file` (favourite-only remote
rows fetch the media into the temp folder first; challenge/login pages
are rejected and retried through the browser, never opened as if they
were the picture), `View metadata…` (readable summary/tags + raw JSON
tree, with `Open JSON in editor…` and `Show in folder`; Overview fills
missing tags/fields from the sidecar, so it never contradicts the JSON
tab), `Hydrus tags…`
(read-only, when enabled and the post has a local file or booru md5),
`Show in folder`
for local files, download/delete and favourite. Esc leaves fullscreen first, then the
viewer; F11 toggles it; arrow keys keep stepping posts while fullscreen.
`Ctrl+D` downloads the post, `Ctrl+O` opens its page, `Ctrl+Shift+O` opens
the media in the default viewer, `F` favourites and `Ctrl+L` jumps back to
the grid omni
bar. The omnibar and filter pills
hide in post view (grid tools would
misclick); they return with the grid. A failed comments/notes fetch shows
a clickable warning (`click for details`) opening the same diagnostics
dialog as media failures. Below the title: `AI` / heart / download
badges mirroring the grid marks. Downloading or (un)favouriting from
the viewer never leaves it; the grid refreshes behind and the viewer
follows it (removed rows resolve to the nearest surviving post,
twin copies collapse so stepping never sticks). Media area: wheel-zoom/drag-pan stills, GIFs,
looping video with seek/volume/rate (the `1.00×` button opens a popup with
a 0.25×–2.00× slider in 0.05× stops plus a `Pitch correction` toggle;
pitch stays constant unless disabled, and the choice is remembered;
playback halts on every post
switch - no background audio, guarded by a wanted-source gate so stale
`LoadedMedia` ticks never restart the previous video from 0:00; audio
follows the system default output device, including sink switches made
while box is running; a set global proxy, or a failed direct
stream, buffers through the download stack instead - same referer +
cookies + proxy as stills; the red error label only appears for terminal
failures, never while still loading/buffering; clicking the red label (or
the viewer error note, or a failed row in the Downloads dialog) opens the
redacted URL + reason like a search error does; any failure also arms the
status-bar probe, so clicking the `Media failed…` line opens the
redacted URL + reason like a search error does; a wall-marked failure
also triggers one silent clearance refresh from the capture profile,
loading the failing media URL (CDN clearances are per host) and then
retrying the post automatically; the `Solve …` button also opens that
media URL, and captured cookies are host-scoped
(`cdn.donmai.us!cf_clearance=…`) so the site and CDN challenges coexist
instead of overwriting each other; walled stills/GIFs
also retry through the browser backend (matching UA + profile cookies),
which re-runs page JS where QNAM replays a dead cookie or gets a
fingerprint 403). Right: metadata (`#id`, site -
downloaded rows show their booru `Label (Engine)` form -, URLs (the
post page, not the downloaded file, except strays),
score, rating with `(inferred)` flag, dimensions, ext) and clickable tag
groups (Artist/Character/Copyright/General/Meta + native custom
categories; right-click a tag for `Add to search` or `Search in
new tab` per booru), then notes + comments on remote posts (long
threads collapse behind a show-all toggle).

## 12-prefs-general - Preferences → General
`Default tab`, `Language`, `Always load last session on startup`,
`Config folder` (effective folder + `Move…`: copies
`settings.ini` + `boorus.json` there, leaves a `[app] configDir` pointer
stub at the default folder; takes effect next launch),
`Archive history` (+ `Clear history`), Favourites toggles
(auto-download/auto-favourite/unfavourite-deletes-file), `LAN sharing`
(`Bind address`, `Port`; defaults for File → Share library on LAN…, which
still asks what to share each start), blacklist box,
and the external handler commands (`External image viewer`, `GIF viewer`,
`video player`, `text editor`; empty = system default, `{file}`/`{url}`
substitution). Bottom: `Defaults` (reset page), `OK` / `Apply` (greys out
when clean - `updateApplyState`) / `Cancel`.

## 13-prefs-library - Preferences → Library
Scan index cache, `Allow duplicate cleanup to delete files in local
archives` (default off; only gates the duplicate dialog), `Media cache` radios (`Cache everything` /
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
`Default boorus` checklist (new tabs + fallbacks), `Data
folder` / `Download folder` (empty = XDG defaults under `box/`),
`Ugoira downloads` (animated webp / animated jxl / keep zip; needs
ffmpeg in PATH, +cjxl for jxl) + `Ugoira frame delay` (fallback when the
API omits pixiv frame timing), `Global proxy` (all traffic; `socks5h://` routes hostnames through
the proxy like curl, per-booru proxy overrides it, `direct` bypasses;
the embedded browser - capture, PoW walls, onion sites - reads it at
startup, so restart after changing it),
Booru
manager: entry list with up/down arrow buttons to reorder + `Name`/`Enabled` (off keeps
the config but hides the booru from every picker)/`short` (grid badge text)/`Badge color`
(known hosts get house colors: gelbooru blue, rule34 green, aibooru green,
danbooru/safebooru tan, shimmie gray; editable anytime)/`Base
URL`/`Type` (engine) editor with cookie capture + favicon override. The
dialog is 780px wide, the editor column 560px, the booru list matches
its two-row button block (Add/Remove above the arrows), and the badge
swatch stays a compact chip: labels, fields and the wrapping cookie row
line up with no dead band between the list and the editor.
Per-entry `Tags` (skip auto-categorization), `Drag posts as` (direct
media vs post page; `Use global` follows Preferences → Browse), `Blacklist` (filter
blacklist locally instead of server `-tags`, for tag-capped accounts),
`Default tags`, `Media cache` (`Global` / `All` / `SFW only` / `Nothing`
for this booru, narrowed by the global mode), `Proxy`.

## 15b-prefs-hydrus - Preferences → Hydrus
`Enable Hydrus tab and tag lookups` (off by default) reveals the Hydrus
tab in the ＋ menu, the stray-editor fetch button and the read-only
context-menu lookup. `Server URL` (a hydrus client API or a
reverse-proxied `hydd`, default `http://127.0.0.1:45869`), `Access key`
(sent as `Hydrus-Client-API-Access-Key`; local `hydd` accepts
empty/bogus keys), `Proxy` (HTTP/SOCKS for Hydrus only; empty = direct
for loopback, the global Browse proxy for remote hosts) and
`Test connection` (`/verify_access_key`, paints the server name + Hydrus
version inline).
`Media folder` and `Database` override the isolated Hydrus storage
(defaults `<data>/hydrus` and `<data>/hydrus.db`; Browse buttons).
`Mask tags below this count` (default 10, 0 = off) drops automatically
fetched Hydrus tags whose network-wide mapping count is lower, using
hydd's `/hyd/tag_counts` extension (the official client and PTR cannot
answer that). `Also mask tags with no Hydrus count` controls tags hydd
reports as undefined. If the extension is disabled/missing/building, tags
are kept unmasked and the tab says so. Manual edits are never masked.

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
Night, Steam Classic, System native), `Accent`, `Custom .qss`, `Sidebar: one scrollbar for all
tag categories` (default off), `Show local folder browser in the quick +
menu` (default off), `Home mascot` Browse/Remove/Reset-to-default +
opacity slider + margin, `Grid` ratio (1:1, 5:4, 4:3, 3:2, 16:9, 21:9,
4:5, 2:3, 3:4, 9:16 or Custom)/scroll, `Snap grid post size to fill the window` (on by
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

## 18-favourites - Favourites tabs (empty state)
Every `Favourites` action opens a fresh tab; keep several with different
tags/filters side by side (titles show their filters). Same chrome over
`store.db` rows (no files): star posts to fill it, then
mass-download; non-empty rows render identical cards, dead files relink
by md5. Tags classify like browse tabs: cached verdicts apply immediately,
new ones fetch once and persist to `store.db`, so the left categories and
the viewer's right-hand tag groups show the same buckets and counts. A
tag appears in exactly one bucket; still-unclassified tags stay under
General instead of disappearing. Favourite rows whose tag columns are
empty (older stray imports, remote/backing-path rows) read their local
sidecar once and persist it, so the tab matches the JSON. `Delete from disk…` also removes kept favourites left with no
renderable source (no file, no remote media/post link) instead of leaving
empty cards; post-linked rows stay (viewer refresh can heal them). Files
removed outside the app hide from the grid the same way (never marked
downloaded) until they return. Remote favourite thumbnails that answer
with a Cloudflare/captcha page retry through the browser backend like the
viewer does, so opening a post is not required to make its card appear;
one capture pass per walled site covers the whole grid. Older relink
passes could store the JSON sidecar as the local file (both share the
stem, and the JSON sorted first): such rows heal to the sibling media on
load/relink instead of opening the JSON in a text editor. Right-click keeps the downloaded-card
actions on favourite-only rows too: `Copy file (clipboard)` (temp fetch
when no local copy), `Copy media link`, `View metadata…` and
`Show in folder` work before any download. File ->
`Redownload missing files…`
re-pulls missing favourites with a download source on demand;
sourceless ones just stay hidden. GUI deletes
also forget the download link, so the action only ever covers files
deleted behind the app's back.

## 19-badge-closeup - origin badge detail
16px site favicon (auto-fetched to `favicons/auto/`, overridable in the
Booru manager) + short name on the entry's badge color. Favourites show
one card per downloaded file even when a remote row and a local (hash)
row both point at it; the older row stays in the database. Favourites rows
store only a display label, so the entry resolves by case-insensitive
label/short name first, then by the post/file URL host (CDN subdomains
match their booru); rows linked to a downloaded copy lose their remote
flag, so the same URL fallback runs for them too (generic engine labels
such as a self-hosted Szuru imported as `Szurubooru` keep their badge).
Badges are hidden on plain single-booru tabs and shown on favourites,
downloads, and multibooru tabs; `G` rating chip, score, `#id`, file name below (synthesized `id - host - tags.ext` when the file URL has no name, e.g. Nuuru `/file`; downloads keep `site_md5` names).

## 20-stray-dialog - Download/Add Stray (`StrayDialog`)
A Booru Explorer feature (not a LoliSnatcher import mode): random URLs
and paid/exclusive uploads that never reach a booru, e.g. Patreon posts.
Big link box (one URL per line) plus `Auto-favourite downloaded/imported
strays`, and four buttons: `Download` (gallery links - Pixiv, Fanbox,
Pawchive, Patreon, … - go through the system gallery-dl package when
found, using its login and credentials but none of its folder or
post-processing choices, landing flat in `_Stray/` as `_Stray_<md5>`
names with basic sidecars it writes itself (box pins the exact download
directory and filters out archives/layered files such as zip and PSD,
then deletes ignored leftovers and any emptied per-site folders; Pixiv
ugoira is kept as a zip and converted by box to animated WebP using the
zip's per-frame delays; its file URL/title/artist metadata is carried
into the sidecar):
page URL recorded, tags left empty for you - one flight
per pasted link; direct media links and recognised booru post pages
keep the built-in path: media fetched as-is, booru pages resolved so
the JSON sidecar carries the real tags - no booru entry is added; without gallery-dl installed, gallery links fall back to the
built-in resolver best-effort), `Favourite
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
editor (flat; a Hydrus fetch writes its namespaced tags straight into the
matching buckets on save, manual additions stay General) with `Fetch from
Hydrus` (when Preferences → Hydrus is enabled: hashes the file SHA-256,
then offers legacy SHA-1/MD5 retries); two or more (`Edit tags…
(N strays)`) open `Mass edit stray tags (N)`: file list, per-bucket boxes
(Artist / Character / Copyright / General / Meta in that order), `Paste
tags:` (`Add pasted` appends tokens to General), `Copy from URL:` +
`Fetch` steals buckets from a booru post link (same resolver as above,
merged into the current file), `Fetch tags from Hydrus` hashes/merges the
current file into the buckets (Hydrus `creator:`/`character:`/`series:`/
`studio:`/`meta:` namespaces fold into their boxes; other tags stay
General), `Copy to all` duplicates the current
file to every listed stray, `Save all` writes `tags` + `tags_*` sidecars
(category order kept). Status bar reports `Saved tags for N stray(s)`.
Any post's right-click menu (grid or viewer) also has read-only
`Hydrus tags…` when a local file or booru md5 can be hashed: grouped
chips, the matched hash kind, the Hydrus file id, `Copy all tags`.

## 21-hydrus-tab - Hydrus library tab (`mainwindow_hydrus.cpp`)
Grid of media from the local Hydrus library (`hydrus.db` + its media
folder). Omnibar searches native tags (`creator:alice`, `character:bob`,
plain General tags; OR-groups work); suggestions come from the library's
own vocabulary. Toolbar: `Import…` (Add files…, Add folder…, Add from
URL(s)… via gallery-dl), `Refresh tags` (selection, else the whole tab;
manual edits skipped), `Not on Hydrus` (only rows whose last lookup
missed), sort box and post count. The rating/category/media pickers,
Boorus…, download and AI buttons are hidden: there is no favourite or
download concept. Files/folders can be dropped on the grid to import.
Sidebar sections are the Hydrus namespaces present in the library
(GENERAL, CREATOR, CHARACTER, SERIES, STUDIO, SPECIES, RATING, META and
anything else), each with its own colour; clicking a tag adds its
namespaced token. Cards honour the same grid size/aspect/stagger
settings; thumbnails stay session-only (never in the booru media cache).
Right-click a card or the open post: `Edit Hydrus tags…`, `Refresh Hydrus
tags`, `Force refresh Hydrus tags…` (includes manual edits and previews
each before/after), `Show in folder`, `Copy file path`, `Copy file
(clipboard)`, `Open file`. The viewer hides favourite/download/comment/
refresh buttons. Import flow: SHA-256 copy as `<sha256>.<ext>` ->
Hydrus lookup -> native tags (masked below the configured count when
hydd's `/hyd/tag_counts` is available) or a per-file manual tag dialog
when Hydrus does not know the file; files already in the folder but
missing from the DB are indexed untagged on open. If counts are
unavailable (official client/PTR, extension disabled, or still building),
every fetched tag is kept and the status bar/summary explains that
masking was skipped.

## 22-lan-sharing - Share library on LAN (`sharedialog`, `shareserver.cpp`)
`File → Share library on LAN…` opens a per-start picker: `Hydrus tab`
(disabled when Hydrus is off/empty), `Online mode: downloads`, `Online
mode: favourites`, and `Local archive folder` with Browse. Any mix is
merged into one read-only Danbooru API; duplicate rows collapse by
site+id, content hash, media URL or path. Network fields: `Bind address`
(default `0.0.0.0` = LAN) and `Port` (default `7071`). Buttons: `Start
sharing` / `Restart with these sources`, `Refresh snapshot` (rebuild while
the listener stays up), `Stop sharing`, `Close`. While live the dialog
shows `API is live`, the post count, the primary URL and `Copy API URL`.
A warning says there is no authentication: anyone who can reach the
address can browse the selected posts.

The API is Danbooru-shaped: `/posts.json?tags=…&page=…&limit=…`,
`/posts/<id>.json`, `/tags.json?search[name_matches]=…` and
`/counts/posts.json`; media at `/share/file/<id>` (Range supported),
previews at `/share/thumb/<id>` (generated and cached for local files,
booru preview for remote-only rows). IDs are stable hash-based numbers.
Local files serve directly; remote-only favourites use the media cache
when enabled and a temporary proxy otherwise; videos stream through Box.
Local thumbnails are cached under `~/.cache/box/share-thumbs`. When the
server runs, a green `API is live` button sits in the bottom-right status
bar; clicking it reopens the picker with the current URL/count. The
listener stops on quit; there is no auto-start.

## 23-duplicate-media - Tools → Find duplicate media… (`dedupdialog`)
The dialog hashes decoded pixels, so tags, sidecars and container format
do not affect matching; lossy JPEG recompression can cross the perceptual
threshold, which is accepted. Exact file bytes (SHA-256) always match.
Matching never crosses a source family (local archive, downloads /
downloaded favourites, Hydrus) or a media type (image, animated first
frame, video), so those libraries do not interact.

Sources: `Downloads / downloaded favourites`, `Hydrus media`, and one or
more `Local archives` (the open archive + recent history, plus
`Add folder…`). `Pixel match tolerance` is the pHash Hamming threshold
(0-16 of 64; default 6). `Scan selected` runs in a worker; the first pass
hashes every file, later passes skip unchanged `(size, mtime)` entries.
`Stop` cancels without pruning; `Forget cached hashes` empties
`dedup.db` (derived data, safe to regenerate). Remote-only favourites are
never downloaded just to dedupe; their downloaded copies are already in
the download family.

Groups show a preview, relative path, size, dimensions, tag count and
family. The starred row is the suggested keeper (most tags, then largest
image, then bytes); select another copy and press `Keep selected copy` to
change it. Every other copy has a delete checkbox. The details pane shows
the full path, hashes and sidecar/Hydrus tags for the selected copy, so
the one with the richer metadata can be kept. `Delete selected…` moves
the media to the trash (falling back to permanent deletion after a second
confirmation), removes both sidecar layouts (`<stem>.json` and
`<media>.json`), and for Hydrus removes the `hydrus.db` row. Archive
copies can only be checked when Preferences → Library → `Allow duplicate
cleanup to delete files in local archives` is on; everything else in Box
still treats archives as read-only.

## Not yet documented (keep unchecked until done)
- [ ] Downloads tab (non-empty: sidecar-backed offline rows)
- [ ] Downloads progress dialog (batch with per-file rows)
- [ ] Cookie capture browser dialog
- [ ] Update-sidecars scope dialog (multi-site)
- [ ] Search-errors inspector (status-bar probe popup)

## 19-theme-files - adding a theme
Themes are data files, like translations: copy `assets/themes/<key>.ini` to a
new `assets/themes/<your-key>.ini` in the repo (reconfigure/build) or drop it
into `~/.config/box/themes/` for runtime discovery. `[theme]` keys:
`name`, `order`, `dark`, `window`, `base`, `alternate`, `button`, `text`,
`dim`, `disabled`, `border`, `accent`. The Preferences combo rebuilds
from those files; a user file with the same name overrides the bundled one.
