#include "runner.h"
#include "migrations.h"
#include "settings.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QProgressDialog>
#include <QSettings>

namespace {

struct Step {
    QString id;
    QString doneKey; // id, or id|dir for dir-scoped steps
    QString title;   // user-visible progress line
    std::function<bool()> needs;
    std::function<bool(QString *)> apply;
};

QVector<Step> buildSteps(AppSettings &settings) {
    // QSettings on the live config file: settings migrations read/write
    // it directly (the in-memory AppSettings was already loaded through
    // the same read fallbacks, so it needs no reload afterwards).
    const QString iniPath = settingsFilePath();
    const QString dataDir = effectiveDataDir(settings);
    const QString cacheBase = appCacheDir();
    const QString storeDb = dataDir + QStringLiteral("/store.db");
    const QString faviconCacheDir = cacheBase + QStringLiteral("/favicons");
    const QString faviconCustomDir = dataDir + QStringLiteral("/favicons");

    // Janitor tracking DB: platform cache dir unless relocated (mirrors
    // CacheJanitor::dbPath without pulling the janitor in; the helper is
    // what janitorDb() itself calls).
    const QString janitorDb = cacheBase + QStringLiteral("/cache.db");

    QVector<Step> steps;
    steps.append({QStringLiteral("settings-keys"),
                  QStringLiteral("settings-keys"),
                  QCoreApplication::translate("Migrations",
                                             "Updating settings…"),
                  [iniPath] {
                      QSettings c(iniPath, QSettings::IniFormat);
                      return Migrations::needsSettingsKeys(c);
                  },
                  [iniPath](QString *e) {
                      QSettings c(iniPath, QSettings::IniFormat);
                      return Migrations::applySettingsKeys(c, e);
                  }});
    steps.append({QStringLiteral("booru-credentials"),
                  QStringLiteral("booru-credentials"),
                  QCoreApplication::translate("Migrations",
                                             "Updating booru registry…"),
                  [iniPath] {
                      QSettings c(iniPath, QSettings::IniFormat);
                      return Migrations::needsLegacyBooruCredentials(c);
                  },
                  [iniPath](QString *e) {
                      QSettings c(iniPath, QSettings::IniFormat);
                      return Migrations::applyLegacyBooruCredentials(c, e);
                  }});
    steps.append({QStringLiteral("cache-layout"),
                  QStringLiteral("cache-layout"),
                  QCoreApplication::translate("Migrations",
                                             "Moving cache folders…"),
                  [cacheBase, dataDir] {
                      return Migrations::needsCacheLayout(cacheBase, dataDir);
                  },
                  [cacheBase, dataDir](QString *e) {
                      return Migrations::applyCacheLayout(cacheBase, dataDir, e);
                  }});
    steps.append({QStringLiteral("favicon-adopt"),
                  QStringLiteral("favicon-adopt|") + dataDir,
                  QCoreApplication::translate("Migrations",
                                             "Moving site icons…"),
                  [faviconCacheDir, faviconCustomDir] {
                      return Migrations::needsFaviconAdopt(faviconCacheDir,
                                                          faviconCustomDir);
                  },
                  [faviconCacheDir, faviconCustomDir](QString *e) {
                      return Migrations::applyFaviconAdopt(faviconCacheDir,
                                                          faviconCustomDir, e);
                  }});
    steps.append({QStringLiteral("favdb-schema"),
                  QStringLiteral("favdb-schema|") + dataDir,
                  QCoreApplication::translate("Migrations",
                                             "Updating favourites database…"),
                  [storeDb] { return Migrations::needsFavDbSchema(storeDb); },
                  [storeDb](QString *e) {
                      return Migrations::applyFavDbSchema(storeDb, e);
                  }});
    steps.append({QStringLiteral("favdb-hash-ids"),
                  QStringLiteral("favdb-hash-ids|") + dataDir,
                  QCoreApplication::translate("Migrations",
                                             "Repairing favourite identities…"),
                  [storeDb] { return Migrations::needsFavDbHashIds(storeDb); },
                  [storeDb](QString *e) {
                      return Migrations::applyFavDbHashIds(storeDb, e);
                  }});
    steps.append({QStringLiteral("janitor-columns"),
                  QStringLiteral("janitor-columns|") + cacheBase,
                  QCoreApplication::translate("Migrations",
                                             "Updating cache tracking…"),
                  [janitorDb] {
                      return Migrations::needsJanitorColumns(janitorDb);
                  },
                  [janitorDb](QString *e) {
                      return Migrations::applyJanitorColumns(janitorDb, e);
                  }});
    return steps;
}

} // namespace

QStringList MigrationRunner::runPending(QWidget *parent, AppSettings &settings) {
    QStringList applied;
    QSettings done(settingsFilePath(), QSettings::IniFormat);
    QStringList doneList =
        done.value(QStringLiteral("migrations/done")).toStringList();
    const int storedVersion =
        done.value(QStringLiteral("migrations/version"), 0).toInt();
    bool dirty = false;

    QVector<Step> pending;
    for (const Step &s : buildSteps(settings)) {
        if (doneList.contains(s.doneKey))
            continue; // ran once already: never probe again
        if (!s.needs()) {
            // Fresh install (or already-new store): nothing to do, but
            // record it so the probe never runs again.
            doneList.append(s.doneKey);
            dirty = true;
            continue;
        }
        pending.append(s);
    }
    if (pending.isEmpty()) {
        if (storedVersion != kVersion) {
            done.setValue(QStringLiteral("migrations/version"), kVersion);
            done.sync();
        } else if (dirty) {
            done.setValue(QStringLiteral("migrations/done"), doneList);
            done.sync();
        }
        return applied;
    }

    // No GUI instance (unit tests) or offscreen platform: run silently.
    const bool guiUp =
        qobject_cast<QGuiApplication *>(QCoreApplication::instance()) !=
        nullptr;
    const bool headless =
        !guiUp ||
        QGuiApplication::platformName() == QStringLiteral("offscreen");
    QProgressDialog *dlg = nullptr;
    if (!headless) {
        const QString label =
            (storedVersion > 0 && storedVersion != kVersion)
                ? QCoreApplication::translate("Migrations",
                                             "Migrating v%1 → v%2…")
                      .arg(storedVersion)
                      .arg(kVersion)
                : QCoreApplication::translate("Migrations",
                                             "Running migrations…");
        dlg = new QProgressDialog(
            label, QString(), 0, pending.size(), parent);
        dlg->setWindowTitle(QCoreApplication::translate("Migrations", "Box"));
        dlg->setCancelButton(nullptr); // not cancellable: DB writes
        dlg->setWindowModality(Qt::ApplicationModal);
        dlg->setMinimumDuration(0);
        dlg->setValue(0);
        dlg->show();
        QCoreApplication::processEvents();
    }

    int i = 0;
    for (const Step &s : pending) {
        if (dlg) {
            dlg->setLabelText(s.title);
            QCoreApplication::processEvents();
        }
        QString error;
        if (s.apply(&error)) {
            applied.append(s.id);
            doneList.append(s.doneKey);
        } else {
            qWarning() << "migration failed:" << s.id << error;
        }
        if (dlg) {
            dlg->setValue(++i);
            QCoreApplication::processEvents();
        }
    }
    done.setValue(QStringLiteral("migrations/done"), doneList);
    done.setValue(QStringLiteral("migrations/version"), kVersion);
    done.sync();
    if (dlg) {
        dlg->setValue(pending.size());
        dlg->close();
        delete dlg;
    }
    return applied;
}
