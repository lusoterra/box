# AGENTS - instructions for future AI sessions

- Read `docs/ARCHITECTURE.md` + `docs/GLOSSARY.md` before touching code;
  `docs/TESTING.md` before building or testing.
- Binary/config/cache/data use `box`: `build/box`, `~/.config/box`,
  `~/.local/share/box`, `~/.cache/box`. Never hardcode `booru-explorer`.
- Verify by execution: run `ctest --test-dir build` / targeted
  `ctest --test-dir build -R <name>` after behavior changes; headless GUI
  work per `docs/TESTING.md`.
- Do not modify code for doc-only tasks; do not build when told read-only.
- Keep docs tight: short bullets, `file:line` refs, no essays, no emojis.
- i18n: wrap new user-visible strings in `tr()`; keep `%1..%9` placeholders
  and `&` accelerators consistent; run the translations check via ctest.

## Docs on UI/behavior changes

- Any UI/behavior change must update the matching `docs/*.md` in the
  same session (`docs/UI-TOUR.md` for visible changes).
- No screenshots: describe UI textually (element names + purposes).

## Migrations (one-time upgrades)

- One-time schema/config/layout upgrades live in `src/migrations/`, one
  file per migration - never inline in constructors or `open()` paths.
  Read `docs/MIGRATIONS.md` before adding or changing one: cheap
  idempotent `needs*` probe, re-runnable `apply*`, register in
  `buildSteps()` (`src/migrations/runner.cpp`), keep the thin fallback
  at the original call site, cover with `test_migrations`.
- The boot dialog (`Running migrations…` / `Migrating vX → vY…`) only
  shows when work is pending; never block startup on it.

## Versioning (semantic, automatic)

- The version lives in one place: `project(Box VERSION x.y.z)` in
  `CMakeLists.txt:2` (`BOX_VERSION` flows to About Box via
  `src/appinfo.h`). Current: `0.6.5`.
- Every behavior-affecting change bumps it in the same commit:
  bugfix -> patch (`x.y.z+1`), backwards-compatible feature -> minor
  (`x.y+1.0`, patch reset to 0). Docs-only / test-only / refactor-only
  changes do not bump.
- Never ever bump the major version (`x`) unless the user explicitly
  asks for it or approves it.

## Feature log (every feature, even minor)

- `docs/PENDING_TASKS.md` and `docs/DONE.md` are local-only logs (gitignored,
  never committed).
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
