#pragma once

// Boot migration runner: runs each pending src/migrations/ step exactly
// once (tracked in settings.ini), with a small progress dialog when there
// is work to do. See docs/MIGRATIONS.md.
#include <QStringList>

struct AppSettings;
class QWidget;

class MigrationRunner {
public:
    // Schema generation of the migration set itself. Bumped when a new
    // migration file lands; the boot dialog then reads
    // "Migrating v<old> -> v<new>…" (see docs/MIGRATIONS.md for the
    // future per-database user_version plan).
    static constexpr int kVersion = 1;

    // Run every pending migration for the current config/data dirs.
    // Shows a modal QProgressDialog only when at least one step needs
    // work (never on the offscreen platform). Returns the ids applied
    // this run (empty = nothing to do). Best effort: a failing step is
    // logged and skipped, never fatal - the original call sites keep
    // idempotent fallbacks.
    static QStringList runPending(QWidget *parent, AppSettings &settings);

private:
    MigrationRunner() = delete;
};
