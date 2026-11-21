# MIGRATIONS - one-time upgrades (`src/migrations/`)

Pre-1.0 the code carried its upgrades inline (DDL on every DB open,
settings fallbacks, file moves in constructors). They now live in one
place: one file per migration, run once at boot with a progress dialog,
never again.

## Layout

- `src/migrations/migrations.h` - every migration's `needs*`/`apply*` contract.
- `src/migrations/runner.h` / `runner.cpp` - `MigrationRunner::runPending()`,
  called from `src/main.cpp` (after the storage safeguard, before the main
  window opens its DBs) and from `MainWindow::applyPreferences`
  (`src/ui/mainwindow_prefs.cpp`) after a data-dir move.
- `src/migrations/m_*.cpp` - one migration each (see table below).

## Contract (follow it for every new migration)

- `needs*()` is a cheap idempotent probe (ini `contains`, `QFile::exists`,
  `PRAGMA table_info`, indexed `SELECT`). It runs on every boot until the
  step is recorded done - keep it to microseconds.
- `apply*()` performs the upgrade and is safe to re-run (`IF NOT EXISTS`,
  guarded renames, one transaction per DB).
- Append the step to `buildSteps()` in `src/migrations/runner.cpp`; bump
  `MigrationRunner::kVersion` when the set changes.
- Global steps use a bare done-key; data/cache-dir steps scope the key
  (`id|dir`) so switching folders re-fires the probe for the new location.
- The original call sites keep thin fallbacks calling the same helpers
  (unit tests never run the runner, they migrate temp DBs on open).
- Wrap the progress title in `translate("Migrations", …)`; run the
  translations check via ctest.

## Boot behavior

- `runPending()` collects steps whose done-key is missing and whose
  `needs*()` hits, then shows a modal `QProgressDialog` (`Running
  migrations…` + per-step title) while applying them. No pending work =
  no dialog. Not cancellable (DB writes).
- When `migrations/version` in `settings.ini` is older than `kVersion`,
  the label reads `Migrating vX → vY…` instead (forward path for the
  per-database `user_version` plan below). Skipped on `offscreen`.
- Applied ids persist under `migrations/done` in `settings.ini`; later
  boots skip them without probing. A failing step logs a warning and
  continues - never fatal (fallbacks still heal lazily).

## Current migrations

| Step (`runner.cpp`) | File | What it consolidates |
|---|---|---|
| `settings-keys` | `m_settings_keys.cpp` | `General/restoreSession→startup/defaultTab`, `General/language→app/language`, `cache/thumbs=false→cache/mode=none`; drops retired `privacy/paranoia`, `search/history` (read fallbacks stay in `src/settings.cpp`). |
| `booru-credentials` | `m_booru_credentials.cpp` | First-run preset seeding, folding legacy `danbooru/user`, `gelbooru/key`, …, `szurubooru/base`, `shimmie/base` onto matching presets; `loadBoorus()` keeps the same-helper fallback (`src/settings.cpp`). |
| `cache-layout` | `m_cache_layout.cpp` | Pre-rename tree `~/.cache/booru-explorer/booru-explorer/*` + data-dir `cache.db` → platform cache dir; `MainWindow` keeps the idempotent call (`src/ui/mainwindow.cpp`). |
| `favicon-adopt` | `m_favicon_adopt.cpp` | Cache-dir `*.png` → `<dataDir>/favicons/auto/`; `FaviconCache::setCustomDir` keeps the call for mid-session data-dir moves (`src/remote/faviconcache.cpp`). |
| `favdb-schema` | `m_favdb_schema.cpp` | `store.db`: `local_path`/`untyped`/`custom` columns + `untyped` backfill; `FavDb::open()` keeps the same-helper call (`src/library/favdb.cpp`). |
| `favdb-hash-ids` | `m_favdb_hash_ids.cpp` | `(site,0)`/qHash rows → stable SHA1 ids; `FavDb::open()` keeps the cheap `post_id=0` probe as a safety net. |
| `janitor-columns` | `m_janitor_columns.cpp` | `cache.db`: `nsfw`/`site` columns; `janitorDb()` keeps the same-helper call (`src/library/cachejanitor.cpp`). |

Deliberately NOT boot migrations (stay where they are):

- `ScanCache` version + explicit-tag invalidation (`src/library/cache.cpp`):
  already versioned per-DB via the `meta` table.
- `seedTagTypesOnce` (`src/ui/mainwindow.cpp`): fresh-DB seeding, guarded
  by file existence - not an upgrade.
- In-memory row healing (`rowToPost` in `src/library/favdb.cpp`): per-read
  guards for imports/old rows, not schema work.

## Future: database versioning

Today the runner versions the *migration set* (`migrations/version`).
When the app goes 1.0, give each database its own `PRAGMA user_version`
and record `migrations/<db>` versions alongside the done-keys, so the
dialog can report per-store progress (`Updating favourites v3 → v4…`).
The step shape already supports it: `needs*` compares the stored version,
`apply*` migrates and bumps it.
