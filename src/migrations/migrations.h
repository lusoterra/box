#pragma once

// Boot migrations: one-time schema/config/layout upgrades, one file per
// migration under src/migrations/ (see docs/MIGRATIONS.md).
//
// Contract per migration (m_*.cpp):
// - needs*() is a cheap idempotent probe (file/ini existence, PRAGMA
//   table_info, indexed SELECT). It must stay fast: it runs on every
//   boot until the step is recorded done, and again after data-dir moves.
// - apply*() performs the upgrade and is safe to re-run (IF NOT EXISTS,
//   guarded renames, single transactions).
// - The original call sites keep thin fallbacks that call the same
//   helpers, so unit tests (which never run the runner) still migrate
//   temp DBs on open.
#include <QString>
#include <functional>

class QSettings;
class QSqlDatabase;

namespace Migrations {

// Legacy ini keys (General/*, cache/thumbs, retired) -> current keys.
// m_settings_keys.cpp
bool needsSettingsKeys(QSettings &c);
bool applySettingsKeys(QSettings &c, QString *error = nullptr);

// First-run preset seeding, folding legacy per-site credentials/bases.
// m_booru_credentials.cpp
bool needsLegacyBooruCredentials(QSettings &c);
bool applyLegacyBooruCredentials(QSettings &c, QString *error = nullptr);

// store.db: local_path/untyped/custom columns + untyped backfill.
// m_favdb_schema.cpp
bool needsFavDbSchema(const QString &dbPath);
bool applyFavDbSchema(const QString &dbPath, QString *error = nullptr);
bool ensureFavDbSchema(QSqlDatabase &db, QString *error = nullptr);

// store.db: unstable (site,0)/qHash identities -> stable SHA1 ids.
// m_favdb_hash_ids.cpp
bool needsFavDbHashIds(const QString &dbPath);
bool applyFavDbHashIds(const QString &dbPath, QString *error = nullptr);
bool healFavDbHashIds(QSqlDatabase &db, QString *error = nullptr);

// cache.db: pre-cache-policy tracking DBs lack nsfw/site columns.
// m_janitor_columns.cpp
bool needsJanitorColumns(const QString &dbPath);
bool applyJanitorColumns(const QString &dbPath, QString *error = nullptr);
bool ensureJanitorColumns(QSqlDatabase &db, QString *error = nullptr);

// Pre-rename on-disk layout: ~/.cache/booru-explorer/booru-explorer/*
// and data-dir cache.db -> platform cache dir. m_cache_layout.cpp
bool needsCacheLayout(const QString &cacheBase, const QString &dataDir);
bool applyCacheLayout(const QString &cacheBase, const QString &dataDir,
                      QString *error = nullptr);

// Portable favicons: adopt auto-fetched cache-dir PNGs into
// <dataDir>/favicons/auto/. m_favicon_adopt.cpp
bool needsFaviconAdopt(const QString &cacheDir, const QString &customDir);
bool applyFaviconAdopt(const QString &cacheDir, const QString &customDir,
                       QString *error = nullptr);

} // namespace Migrations
