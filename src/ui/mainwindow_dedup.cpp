#include "mainwindow.h"

#include "dedupdialog.h"
#include "hydrusstore.h"
#include "settings.h"

#include <QDir>
#include <QFileInfo>
#include <QStatusBar>

namespace {
bool under(const QString &path, const QString &dir) {
    if (path.isEmpty() || dir.isEmpty())
        return false;
    const QString d = QDir(dir).canonicalPath();
    if (d.isEmpty())
        return false;
    const QString p = QFileInfo(path).canonicalFilePath();
    const QString use = p.isEmpty() ? QFileInfo(path).absoluteFilePath() : p;
    return use == d || use.startsWith(d + u'/');
}
} // namespace

void MainWindow::openDedupDialog() {
    if (m_dedupDialog) {
        m_dedupDialog->raise();
        m_dedupDialog->activateWindow();
        return;
    }
    if (m_settings.hydrusEnabled)
        ensureHydrusDb();

    DedupDialogSources src;
    src.dbPath = effectiveDataDir(m_settings) + QStringLiteral("/dedup.db");
    src.downloadDir = effectiveDownloadDir(m_settings);
    src.hydrusDir = effectiveHydrusDir(m_settings);
    src.hydrusDbPath = effectiveHydrusDbPath(m_settings);
    src.allowArchiveDelete = m_settings.dedupAllowArchiveDelete;

    QStringList roots;
    const auto addRoot = [&roots](const QString &path) {
        if (path.isEmpty() || !QDir(path).exists())
            return;
        const QString canon = QDir(path).canonicalPath();
        if (!canon.isEmpty() && !roots.contains(canon))
            roots << canon;
    };
    BrowserTab *t = currentTab();
    if (t && !t->isRemote && !t->isFavourites && !t->isHydrus) {
        addRoot(t->root);
        src.currentRoot = t->root;
    }
    addRoot(m_settings.lastRoot);
    for (const QString &root : m_settings.history)
        addRoot(root);
    src.archiveRoots = roots;

    auto *dlg = new DedupDialog(
        src, m_hydrusDb.isOpen() ? &m_hydrusDb : nullptr, this);
    m_dedupDialog = dlg;
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    connect(dlg, &DedupDialog::filesDeleted, this,
            &MainWindow::onDedupFilesDeleted);
    dlg->show();
}

void MainWindow::onDedupFilesDeleted(const QStringList &paths,
                                     const QStringList &archiveRoots) {
    invalidateDownloadIndex();
    refreshFavTabs();
    repaintFavMarks();
    updateViewerBadges();

    const QString downloadDir = effectiveDownloadDir(m_settings);
    bool downloadTouched = false;
    for (const QString &path : paths) {
        if (under(path, downloadDir)) {
            downloadTouched = true;
            break;
        }
    }

    QStringList affected = archiveRoots;
    if (downloadTouched)
        affected << downloadDir;
    for (const QString &root : affected) {
        const QString canon = QDir(root).canonicalPath();
        m_dirtyRoots.insert(canon.isEmpty() ? root : canon);
    }

    BrowserTab *t = currentTab();
    if (t && t->isHydrus) {
        loadHydrusTab(t);
    } else if (t && !t->isRemote && !t->isFavourites) {
        const QString current = QDir(t->root).canonicalPath();
        for (const QString &root : affected) {
            if (under(current, root) || under(root, current)) {
                startScan(t->root);
                break;
            }
        }
    }
    statusBar()->showMessage(
        tr("Deleted %1 duplicate file(s)").arg(paths.size()), 6000);
}
