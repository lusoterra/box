#include "mainwindow.h"
#include "appinfo.h"
#include "cache.h"
#include "cachejanitor.h"
#include "cachepolicy.h"
#include "cookiegrab.h"
#include "downloadmanager.h"
#include "filehash.h"
#include "imagecache.h"
#include "losnimport.h"
#include "metaparser.h"
#include "prefsdlg.h"
#include "progressbar.h"
#include "queryor.h"
#include "remotesource.h"
#include "scanner.h"
#include "settingsexport.h"
#include "stray.h"
#include "straydialog.h"
#include "strayresolve.h"
#include "tabbar.h"
#include "faviconcache.h"
#include "sidecarrefresh.h"
#include "tagsuggest.h"
#include "theme.h"
#include "uicons.h"
#include "scanner.h"
#include "tagtypes.h"
#include "theme.h"
#include "thumbloader.h"
#include "viewerpage.h"
#include "welcomepage.h"
#include <QAbstractItemView>
#include <QApplication>
#include <QActionGroup>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QCompleter>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QDrag>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QWidgetAction>
#include <QMenuBar>
#include <QMessageBox>
#include <QProcess>
#include <QMimeData>
#include <QPainter>
#include <QPushButton>
#include <QProgressBar>
#include <QProgressDialog>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QShortcut>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStringListModel>
#include <QStyledItemDelegate>
#include <QTemporaryFile>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QtConcurrent>
#include <climits>
#include <functional>
#include <utility>
#include <algorithm>

void MainWindow::updateSortBox() {
    // Order-preserving sorts are always safe; the reordering ones need
    // every tab site to translate them server-side (sortSearchTag), else
    // infinite-scroll appends reshuffle the grid on every page.
    static const QSet<QString> kStable = {QStringLiteral("added"),
                                          QStringLiteral("id-desc")};
    QStringList sites;
    if (const BrowserTab *t = currentTab(); t && t->isRemote)
        sites = t->sites.isEmpty() ? m_settings.browseSites : t->sites;
    auto *model = qobject_cast<QStandardItemModel *>(m_sortBox->model());
    if (!model)
        return;
    for (int i = 0; i < m_sortBox->count(); ++i) {
        const QString v = m_sortBox->itemData(i).toString();
        bool ok = kStable.contains(v);
        if (!ok && !sites.isEmpty()) {
            ok = true;
            for (const QString &key : sites) {
                if (isVirtualSource(key))
                    continue;
                const BooruSite *e = RemoteSource::siteFor(key);
                if (!e ||
                    RemotePosts::sortSearchTag(v, e->engine).isEmpty()) {
                    ok = false;
                    break;
                }
            }
        }
        if (QStandardItem *item = model->item(i))
            item->setEnabled(ok);
    }
    // A now-disabled selection falls back to server order (no reshuffle).
    const int cur = m_sortBox->currentIndex();
    if (cur >= 0 && model->item(cur) && !model->item(cur)->isEnabled()) {
        const int added = m_sortBox->findData(QStringLiteral("added"));
        m_sortBox->blockSignals(true);
        m_sortBox->setCurrentIndex(qMax(0, added));
        m_sortBox->blockSignals(false);
        m_sort = QStringLiteral("added");
    }
}

void MainWindow::rebuildSitesMenu() {
    m_sitesMenu->clear();
    for (const BooruSite &s : m_settings.boorus) {
        if (!s.enabled)
            continue;
        QAction *a = m_sitesMenu->addAction(s.label);
        a->setCheckable(true);
        a->setData(s.key);
    }
    // Virtual sources behave like boorus here too (merge into browse tabs).
    // Plain text: the action icons already carry the marks, no emoji needed.
    m_sitesMenu->addSeparator();
    QAction *favAct =
        m_sitesMenu->addAction(UiIcons::heart(UiIcons::favRed(), true),
                               tr("Favourites"));
    favAct->setCheckable(true);
    favAct->setData(kSrcFavourites);
    QAction *dlAct = m_sitesMenu->addAction(
        UiIcons::icon(QStringLiteral("download")), tr("Downloads"));
    dlAct->setCheckable(true);
    dlAct->setData(kSrcDownloads);
    syncSitesMenu();
    updateSortBox();
}

void MainWindow::applyBooruCreds() {
    // Tag classifier + notes/comments fetchers follow the registry:
    // whatever is configured on those entries (possibly nothing).
    const BooruSite *gel = RemoteSource::siteFor(QStringLiteral("gelbooru"));
    const QString gelUser = gel ? gel->user : QString();
    const QString gelKey = gel ? gel->secret : QString();
    m_fetcher->setGelbooruCredentials(gelUser, gelKey);
    m_browseFetcher->setGelbooruCredentials(gelUser, gelKey);
    const BooruSite *dan = RemoteSource::siteFor(QStringLiteral("danbooru"));
    m_viewer->setMetaCreds(dan ? dan->user : QString(),
                           dan ? dan->secret : QString(), gelUser, gelKey);
}

void MainWindow::updateDownloadSidecars() {
    const QString dest = effectiveDownloadDir(m_settings);
    // Scope first: one checkbox per site subfolder holding refreshable
    // sidecars (all checked). Single-site libraries skip the dialog.
    const QList<SidecarRefresh::Job> all =
        SidecarRefresh::collect(dest, RemoteSource::labelKeys());
    if (all.isEmpty()) {
        statusBar()->showMessage(tr("Nothing to refresh"), 4000);
        return;
    }
    QMap<QString, int> counts;
    for (const SidecarRefresh::Job &j : all) {
        const QString label =
            QFileInfo(j.jsonPath).dir().dirName();
        counts[label]++;
    }
    QSet<QString> scope;
    if (counts.size() > 1) {
        QDialog dlg(this);
        dlg.setWindowTitle(tr("Update downloaded sidecars"));
        auto *lay = new QVBoxLayout(&dlg);
        lay->addWidget(new QLabel(tr("Refresh sidecars for:"), &dlg));
        auto *list = new QListWidget(&dlg);
        for (auto it = counts.begin(); it != counts.end(); ++it) {
            auto *item = new QListWidgetItem(
                QStringLiteral("%1 (%2)").arg(it.key()).arg(it.value()),
                list);
            item->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled);
            item->setCheckState(Qt::Checked);
            item->setData(Qt::UserRole, it.key());
        }
        lay->addWidget(list, 1);
        auto *buttons = new QDialogButtonBox(
            QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
        lay->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
        dlg.resize(320, 300);
        if (dlg.exec() != QDialog::Accepted)
            return;
        for (int i = 0; i < list->count(); ++i) {
            const QListWidgetItem *item = list->item(i);
            if (item->checkState() == Qt::Checked)
                scope.insert(item->data(Qt::UserRole).toString());
        }
        if (scope.isEmpty())
            return;
    } else {
        scope = QSet<QString>(counts.keyBegin(), counts.keyEnd());
    }
    // Reuse the collected jobs (no second tree walk): keep the scope.
    QList<SidecarRefresh::Job> jobs;
    for (const SidecarRefresh::Job &j : all) {
        if (scope.contains(QFileInfo(j.jsonPath).dir().dirName()))
            jobs.append(j);
    }
    runSidecarJobs(dest, scope, jobs);
}

void MainWindow::runSidecarJobs(const QString &dest, const QSet<QString> &scope,
                                QList<SidecarRefresh::Job> jobs) {
    auto *batch = new SidecarBatch(this);
    auto *prog = new QProgressDialog(tr("Updating sidecars…"), tr("Cancel"),
                                     0, 0, this);
    prog->setAttribute(Qt::WA_DeleteOnClose);
    prog->setMinimumDuration(500);
    prog->setWindowTitle(tr("Update downloaded sidecars"));
    connect(batch, &SidecarBatch::progress, prog,
            [prog](int done, int total, const QString &current) {
                prog->setRange(0, qMax(1, total));
                prog->setValue(done);
                prog->setLabelText(current.isEmpty()
                    ? tr("Updating sidecars…")
                    : tr("Updating %1…").arg(current));
            });
    connect(batch, &SidecarBatch::finished, prog,
            [this, prog, batch, dest, scope](int updated, int failed,
                                             int skipped) {
                batch->deleteLater();
                prog->close();
                statusBar()->showMessage(
                    tr("Sidecars updated: %1 ok, %2 failed, %3 skipped")
                        .arg(updated)
                        .arg(failed)
                        .arg(skipped),
                    8000);
                // Rescan when the current local tab lives inside the
                // refreshed scope (the whole tree when unscoped).
                if (updated > 0 && m_curTab >= 0 && !m_tabs[m_curTab]->isRemote &&
                    !m_tabs[m_curTab]->isFavourites) {
                    const QString tabRoot =
                        QDir(m_tabs[m_curTab]->root).canonicalPath();
                    bool covered = scope.isEmpty();
                    for (const QString &label : scope) {
                        const QString dir =
                            QDir(dest + u'/' + label).canonicalPath();
                        if (!dir.isEmpty() &&
                            (tabRoot == dir ||
                             tabRoot.startsWith(dir + u'/') ||
                             dir.startsWith(tabRoot + u'/'))) {
                            covered = true;
                            break;
                        }
                    }
                    if (covered)
                        startScan(m_root);
                }
            });
    connect(prog, &QProgressDialog::canceled, batch, &SidecarBatch::cancel);
    connect(batch, &SidecarBatch::finished, batch, &SidecarBatch::deleteLater);
    batch->startJobs(std::move(jobs));
    if (!batch->isActive()) {
        // Nothing collectible: start() already emitted finished.
        prog->close();
        batch->deleteLater();
    }
}

