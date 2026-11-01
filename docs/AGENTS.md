# AGENTS - instructions for future AI sessions

- Read `docs/ARCHITECTURE.md` + `docs/GLOSSARY.md` before touching code;
  `docs/TESTING.md` before building or testing.
- Binary/config/cache/data use `box`: `build/box`, `~/.config/box`,
  `~/.local/share/box`, `~/.cache/box`. Never hardcode `booru-explorer`.
- Verify by execution: run `make test` / targeted `ctest -R <name>` after
  behavior changes; headless GUI work per `docs/TESTING.md`.
- Do not modify code for doc-only tasks; do not build when told read-only.
- Keep docs tight: short bullets, `file:line` refs, no essays, no emojis.
- i18n: wrap new user-visible strings in `tr()`; keep `%1..%9` placeholders
  and `&` accelerators consistent; run the translations check via ctest.

## Docs on UI/behavior changes

- Any UI/behavior change must update the matching `docs/*.md` in the
  same session (`docs/UI-TOUR.md` for visible changes).
- No screenshots: describe UI textually (element names + purposes).

## Feature log (every feature, even minor)

- Every new feature - no matter how small - gets a `docs/PENDING_TASKS.md`
  entry before coding: what it is supposed to do, where it lives
  (`file:line`), which tests cover it, and the `ctest` result. Minor
  bugfixes get one line; bigger features get a `## N. Title - DONE`
  block with ticked sub-boxes.
- Purpose: stop regressions and minor annoying bugs coming back. If the
  entry cannot name a test, say why (e.g. needs live network / manual
  Xvfb eyes) and what was verified by execution instead.
- Keep the canonical copy in `docs/`; tick boxes as each lands with tests
  passing. Follow the existing style: short bullets, `file:line` refs, no
  essays, no emojis.
