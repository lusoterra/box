# LoliSnatcher parity notes (surveyed 2026-10-21, mirror `git.lusosterone.net/mirrors/LoliSnatcher_Droid`)

Source of truth for what to adopt vs skip. Engine picks stay with the
user; everything else follows the tags below.

- ADOPT (filed as follow-up tasks below): fits the desktop/offline
  ethos, small, clearly missing.
- WONT-DO: documented with a one-line reason.
- PICK: user decides (usually whole engine families).

## Engines

| LoliSnatcher has | box has | note |
|---|---|---|
| Danbooru / Gelbooru / Moebooru / Shimmie / Szurubooru | yes | — |
| Safebooru-as-dapi | yes (box concept, no losn equiv) | — |
| HyBooru | yes (box concept) | — |
| e621 | no | PICK: biggest missing engine (furry). |
| Philomena / BooruOnRails (Twibooru) | no | PICK: pony family. |
| Sankaku (+Idol) | no | PICK: needs account + anti-bot handling. |
| R34Hentai / R34US / Realbooru / RainBooru / AGNPH / InkBunny / WildCritters / NyanPals / WorldXYZ | no | PICK, mostly scrapers/meme hosts; box covers meme hosts via `skipAutoClassify` instead. |
| Hydrus-as-source | no | WONT-DO: Hydrus is a local client, not a web booru; box reads local archives directly. |
| GelbooruV1 (legacy XML) | no (Gelbooru dapi covers old builds) | WONT-DO unless a live instance needs it. |
| Merge pseudo-type | yes (multibooru secondary sites) | parity |
| Downloads / Favourites pseudo-tabs | yes (Downloads folder tab, fav tabs) | parity |

Per-entry config: box is AHEAD (per-entry proxy, cookie browser-capture,
badge colours, disabled buckets, skip-auto-classify, default tags).
LoliSnatcher has only name/type/URL/creds/defTags + one global proxy.
Share-to-Hydrus: WONT-DO (no Hydrus target on desktop; Open post covers it).

## Search / filter

- ADOPT: none filed (box is ahead: OR-groups, pills, default-tag cards,
  media dropdown, favourite-tag priority).
- PICK: search-history page (box: none; full-page list, pin, multi-delete).
- PICK: structured metatag editors (per-booru `availableMetaTags`,
  typed bool/number/date/sort editors). Box covers the common ones via
  typed tags + dropdowns; the editor UI is the gap.
- WONT-DO: tag-manager with background type-fetch queue (box resolves
  verdicts into the tag-type cache on demand instead; equivalent outcome).
- WONT-DO: hated/marked runtime filters beyond blacklist (box blacklist
  + AI toggle + rating cover the same ground).

## Viewer

- ADOPT (filed): video playback speed control.
- ADOPT (filed): settings backup/restore JSON export/import (boorus,
  settings, fav tags, favourites; mirrors LoliSync's backup channel
  without any server).
- PICK: external-player button, GIF-as-thumbnail toggle.
- WONT-DO: LoliSync device sync (desktop single-user; no server
  component wanted) — the export file above is the offline answer.
- WONT-DO: biometric lock, update checker, Discord button, onboarding
  tours, wakelock/vibration/incognito-keyboard (mobile-only concerns).
- Reverse search: box is AHEAD (adds IQDB + trace.moe; losn adds ImgOps
  + plain Google — not worth chasing).
- Notes: parity (both overlay + toggle). Comments: box inline-collapsed
  vs losn dialog — equivalent, keep box's.

## Downloads

- Box is AHEAD: unfavourite-deletes-file, redownload-missing sweep,
  stray import, path-reverse-map, local-path relink.
- LoliSnatcher extras: stuck-watchdog auto-retry with retry buckets UI,
  per-snatch cooldown, custom filename templates, per-booru download
  subfolder override, download notifications. PICK as a batch (download
  robustness pack) — none filed individually.
- Coupling toggles both directions: parity (`snatchOnFavourite` =
  auto-download; box fav-auto-download defaults OFF, losn defaults differ
  — keep box defaults).

## Settings / theming

- Box is AHEAD where it counts for desktop (booru manager depth,
  cache controls, tag-type overrides, mascot).
- WONT-DO: Material-3 dynamic colours, font picker, launcher-icon
  spoofing, rotation lock, predictive back, desktop/mobile shell
  duality (Flutter-mobile concerns, not Qt desktop).
- PICK: gallery layout modes (grid/waterfall/staggered), thumbnail
  preload tuning (overlaps task 28 windowing work — decide there).

## Tabs

- Parity with different strengths: losn has tab-manager filters,
  page-jump, merge/replace-via-sync, secondary-booru sets; box has
  local-archive tabs, last-place resume, session restore.
- WONT-DO: merge/replace-via-sync (needs LoliSync).
- PICK: tab-manager filter dialog, per-tab page jump.
