# AGENTS — box (BOoru eXplorer)

Read before touching code:
- `docs/ARCHITECTURE.md` + `docs/GLOSSARY.md` — module map, data flows, spoken-term mappings
- `docs/TESTING.md` — before building, testing, or headless GUI work
- `docs/UI-TOUR.md` — what each menu/dialog contains

Conventions: verify by execution (`make test` / `ctest -R <name>`); short
bullets with `file:line` refs, no essays, no emojis. Binary/config/data
use `box` (`build/box`, `~/.config/box`, `~/.local/share/box`,
`~/.cache/box`) — never hardcode `booru-explorer` except legacy-dir fallbacks.

UI/behavior changes must update `docs/*.md` in the same session.
