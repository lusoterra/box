# AGENTS - box (BOoru eXplorer)

Read before touching code:
- `docs/ARCHITECTURE.md` + `docs/GLOSSARY.md` - module map, data flows, spoken-term mappings
- `docs/TESTING.md` - before building, testing, or headless GUI work
- `docs/UI-TOUR.md` - what each menu/dialog contains

Conventions: verify by execution (`make test` / `ctest -R <name>`); short
bullets with `file:line` refs, no essays, no emojis. Binary/config/data
use `box` (`build/box`, `~/.config/box`, `~/.local/share/box`,
`~/.cache/box`) - never hardcode `booru-explorer` except legacy-dir fallbacks.

UI/behavior changes must update `docs/*.md` in the same session.

Reference project: **LoliSnatcher** (`lolisnatcher`/`LoliSnatcher_Droid`, the
Dart/Flutter booru client this app is inspired by). Use it as the design
reference for navigation, viewer stepping, session restore, grid scrolling,
favourites/downloads and browsing flow; check its logic before inventing a
parallel one. It is a reference only - do not copy unrelated code or assets.

Massive prompts and fast firing small prompts with bugs to fix etc, should be
added to `docs/PENDING_TASKS.md` before and done one by one as to not get lost.
The root `icon.png` is gone: the source icon is `assets/box.svg` (embedded as
`:/icons/app.svg`); update the SVG, not a raster copy.

Do not change hand-written things from README (first few paragraphs), and keep
the License paragraph in the very bottom.

## Testing hygiene (cache/booru)

- When testing the software, prefer SFW boorus. For boorus that allow
  NSFW content, only cache SFW stuff - do not litter the machine with
  NSFW media.
- Cache test artifacts under `/tmp/opencode` (erased on reboot); if space
  is tight there, avoid large downloads or trim the cache rather than
  spilling elsewhere.
