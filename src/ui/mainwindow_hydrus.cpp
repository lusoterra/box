#include "mainwindow.h"

#include "desktopfiles.h"
#include "filehash.h"
#include "gallerydl.h"
#include "hydrus.h"
#include "hydrusdialog.h"
#include "hydrusstore.h"
#include "hydrustagdialog.h"
#include "queryor.h"
#include "straydialog.h"
#include "tabbar.h"
#include "ugoioconvert.h"
#include "uicons.h"

#include <QApplication>
#include <QClipboard>
#include <QCompleter>
#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QInputDialog>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QProgressDialog>
#include <QPushButton>
#include <QSet>
#include <QStandardPaths>
#include <QStatusBar>
#include <QSplitter>
#include <QTimer>
#include <QUrl>
#include <QUuid>
#include <QVBoxLayout>
#include <QtConcurrent>

#include <algorithm>

namespace {

const QStringList &hydrusMediaExtensions() {
    static const QStringList exts = {
        QStringLiteral("png"),  QStringLiteral("jpg"),
        QStringLiteral("jpeg"), QStringLiteral("gif"),
        QStringLiteral("webp"), QStringLiteral("avif"),
        QStringLiteral("jxl"),  QStringLiteral("mp4"),
        QStringLiteral("webm"), QStringLiteral("bmp"),
        QStringLiteral("tif"),  QStringLiteral("tiff")};
    return exts;
}

bool isHydrusMedia(const QString &path) {
    return hydrusMediaExtensions().contains(
        QFileInfo(path).suffix().toLower());
}

// Zip is not Hydrus media by itself, but Pixiv ugoira arrives as a zip and
// is converted before staging. Keep it in the collector so conversion gets
// a chance; stageHydrusFiles still refuses any leftover archive.
bool isImportCandidate(const QString &path) {
    const QString ext = QFileInfo(path).suffix().toLower();
    return isHydrusMedia(path) || ext == QStringLiteral("zip");
}

// Color bucket for a native Hydrus namespace. Unknown namespaces use the
// accent "custom" colour so nothing is ever hidden or folded away.
QString namespaceBucket(const QString &ns) {
    if (ns.isEmpty())
        return QStringLiteral("general");
    const QString n = ns.toLower();
    if (n == QStringLiteral("creator") || n == QStringLiteral("artist") ||
        n == QStringLiteral("person"))
        return QStringLiteral("artist");
    if (n == QStringLiteral("character") || n == QStringLiteral("char"))
        return QStringLiteral("character");
    if (n == QStringLiteral("series") || n == QStringLiteral("copyright") ||
        n == QStringLiteral("studio") || n == QStringLiteral("franchise"))
        return QStringLiteral("copyright");
    if (n == QStringLiteral("species"))
        return QStringLiteral("species");
    if (n == QStringLiteral("rating"))
        return QStringLiteral("rating");
    if (n == QStringLiteral("meta") || n == QStringLiteral("title") ||
        n == QStringLiteral("filename") || n == QStringLiteral("source"))
        return QStringLiteral("meta");
    return QStringLiteral("custom");
}

QString namespaceTitle(const QString &ns) {
    if (ns.isEmpty())
        return QStringLiteral("GENERAL");
    QString s = ns;
    s.replace(u'_', u' ');
    return s.toUpper();
}

QStringList collectMediaFiles(const QStringList &paths) {
    QStringList files;
    QSet<QString> seen;
    for (const QString &raw : paths) {
        const QFileInfo fi(raw);
        if (fi.isDir()) {
            QDirIterator it(fi.absoluteFilePath(), QDir::Files,
                            QDirIterator::Subdirectories);
            while (it.hasNext()) {
                const QString p = it.next();
                if (!isImportCandidate(p) || seen.contains(p))
                    continue;
                seen.insert(p);
                files << p;
            }
        } else if (fi.isFile() && isImportCandidate(raw) &&
                   !seen.contains(raw)) {
            seen.insert(raw);
            files << raw;
        }
    }
    return files;
}

// Worker: hash, de-duplicate by SHA-256 and copy into the hydrus folder.
// The DB is never touched here (connections are per-thread).
QVector<HydrusStore::StagedFile> stageHydrusFiles(
    const QStringList &paths, const QString &destDir, QSet<QString> known) {
    QVector<HydrusStore::StagedFile> out;
    QDir().mkpath(destDir);
    for (const QString &src : paths) {
        const QFileInfo fi(src);
        if (!fi.isFile() || !isHydrusMedia(src))
            continue;
        HydrusStore::StagedFile r;
        r.source = src;
        r.ext = fi.suffix().toLower();
        r.sha256 = fileContentSha256(src);
        if (r.sha256.isEmpty()) {
            r.error = QObject::tr("Cannot read file");
            out << r;
            continue;
        }
        if (known.contains(r.sha256)) {
            r.duplicate = true;
            out << r;
            continue;
        }
        const QString name =
            HydrusStore::fileNameForHash(r.sha256, r.ext);
        r.target = QDir(destDir).filePath(name);
        if (QFileInfo::exists(r.target)) {
            // Already in the folder (an orphan the DB has not seen yet):
            // not a copy, but never overwrite bytes.
            r.duplicate = true;
            out << r;
            continue;
        }
        const QString part = r.target + QStringLiteral(".part");
        QFile::remove(part);
        if (!QFile::copy(src, part)) {
            r.error = QObject::tr("Cannot copy into the Hydrus folder");
            out << r;
            continue;
        }
        QFile::remove(r.target);
        if (!QFile::rename(part, r.target)) {
            QFile::remove(part);
            r.error = QObject::tr("Cannot move the copy into place");
            out << r;
            continue;
        }
        known.insert(r.sha256);
        out << r;
    }
    return out;
}

// Worker: find media already on disk in the hydrus folder without a
// hydrus.db row. Files named <sha256>.<ext> trust their name (no re-read);
// anything else is hashed.
QVector<HydrusStore::Record> scanHydrusOrphans(const QString &dir,
                                               QSet<QString> known) {
    QVector<HydrusStore::Record> out;
    if (dir.isEmpty() || !QDir(dir).exists())
        return out;
    QDirIterator it(dir, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        if (!isHydrusMedia(path))
            continue;
        const QFileInfo fi(path);
        const QString stem = fi.completeBaseName();
        QString sha = HydrusStore::isSha256Hex(stem) ? stem.toLower()
                                                     : fileContentSha256(path);
        if (sha.isEmpty() || known.contains(sha))
            continue;
        known.insert(sha);
        HydrusStore::Record r;
        r.sha256 = sha;
        r.fileName = fi.fileName();
        r.ext = fi.suffix().toLower();
        r.bytes = fi.size();
        r.addedMs = fi.lastModified().toMSecsSinceEpoch();
        r.source = path;
        r.found = false;
        out << r;
    }
    return out;
}

// Media files under a directory (gallery-dl staging).
QStringList mediaUnder(const QString &dir) {
    return collectMediaFiles({dir});
}

} // namespace

bool MainWindow::isHydrusTab() {
    const BrowserTab *t = currentTab();
    return t && t->isHydrus;
}

bool MainWindow::ensureHydrusDb() {
    const QString dbPath = effectiveHydrusDbPath(m_settings);
    if (m_hydrusDb.isOpen() && m_hydrusDb.path() == dbPath)
        return true;
    m_hydrusDb.close();
    const QString dir = effectiveHydrusDir(m_settings);
    if (!QDir().mkpath(dir)) {
        QMessageBox::warning(
            this, tr("Hydrus"),
            tr("Cannot create the Hydrus media folder:\n%1").arg(dir));
        return false;
    }
    if (!m_hydrusDb.open(dbPath)) {
        QMessageBox::warning(
            this, tr("Hydrus"),
            tr("Cannot open the Hydrus database:\n%1").arg(dbPath));
        return false;
    }
    return true;
}

void MainWindow::openHydrusTab() {
    if (!m_settings.hydrusEnabled) {
        QMessageBox::information(
            this, tr("Hydrus"),
            tr("Enable Hydrus integration in Preferences → Hydrus first."));
        return;
    }
    if (!ensureHydrusDb())
        return;
    allowNetwork();
    auto *t = new BrowserTab();
    t->isHydrus = true;
    t->sort = QStringLiteral("added");
    t->title = tr("Hydrus");
    m_tabs.append(t);
    m_tabBar->addTab(QString());
    updateTabTitle(static_cast<int>(m_tabs.size()) - 1);
    m_tabBar->setCurrentIndex(static_cast<int>(m_tabs.size()));
    saveTabsNow();
}

void MainWindow::loadHydrusTab(BrowserTab *t) {
    if (!t || !t->isHydrus)
        return;
    if (!ensureHydrusDb()) {
        m_scanBar->setVisible(false);
        hideTabLoading(t);
        return;
    }
    const QString dir = effectiveHydrusDir(m_settings);
    const QVector<HydrusStore::Record> rows = m_hydrusDb.list();
    t->localPosts.clear();
    t->localPosts.reserve(rows.size());
    m_hydrusMissing.clear();
    for (const HydrusStore::Record &r : rows) {
        t->localPosts.append(r.toPost(dir));
        if (!r.found)
            m_hydrusMissing.insert(r.sha256);
    }
    m_scanBar->setVisible(false);
    setSidebarLoading(false);
    m_countLabel->setText(tr("%1 posts").arg(t->localPosts.size()));
    updateDots();
    rebuildHydrusSidebar();
    applyHydrusFilter();
    hideTabLoading(t);
    maybeScanHydrusOrphans(false);
}

void MainWindow::applyHydrusFilter() {
    BrowserTab *t = currentTab();
    if (!t || !t->isHydrus)
        return;
    const QueryOr::ParsedQuery pq = QueryOr::parse(t->searchQuery);
    QStringList inc, exc;
    splitIncExc(t->activeTags + pq.andTokens, inc, exc);
    QVector<Post> out;
    for (const Post &p : t->localPosts) {
        if (t->hydrusOnlyMissing && !m_hydrusMissing.contains(p.sha256))
            continue;
        if (!matchesTokens(p, inc, exc))
            continue;
        if (!groupsMatch(p, pq.groups))
            continue;
        out.append(p);
    }
    // The shared sort box maps onto Hydrus meanings: id = import date,
    // score = tag count. "added" keeps the DB's newest-first order.
    const auto tagCount = [](const Post &p) {
        int n = 0;
        for (auto it = p.customTags.begin(); it != p.customTags.end(); ++it)
            n += static_cast<int>(it.value().size());
        n += static_cast<int>(
            p.tagsGeneral.split(QChar::Space, Qt::SkipEmptyParts).size());
        return n;
    };
    if (t->sort == QStringLiteral("id-asc")) {
        std::stable_sort(out.begin(), out.end(),
                         [](const Post &a, const Post &b) {
                             return a.date < b.date;
                         });
    } else if (t->sort == QStringLiteral("id-desc")) {
        std::stable_sort(out.begin(), out.end(),
                         [](const Post &a, const Post &b) {
                             return a.date > b.date;
                         });
    } else if (t->sort == QStringLiteral("score-desc")) {
        std::stable_sort(out.begin(), out.end(),
                         [&tagCount](const Post &a, const Post &b) {
                             return tagCount(a) > tagCount(b);
                         });
    } else if (t->sort == QStringLiteral("score-asc")) {
        std::stable_sort(out.begin(), out.end(),
                         [&tagCount](const Post &a, const Post &b) {
                             return tagCount(a) < tagCount(b);
                         });
    } else if (t->sort == QStringLiteral("random")) {
        sortPosts(out, QStringLiteral("random"));
    }
    m_current = out;
    fillModel();
    updateResultLabel();
    updateTabTitle(m_curTab);
}

void MainWindow::rebuildHydrusSidebar() {
    BrowserTab *t = currentTab();
    if (!t || !t->isHydrus)
        return;
    setSidebarLoading(false);
    // The booru five do not exist for Hydrus: native namespaces replace
    // them entirely.
    for (int i = 0; i < 5 && i < m_sideSplit->count(); ++i)
        m_sideSplit->widget(i)->setVisible(false);
    for (auto it = m_customSections.begin(); it != m_customSections.end(); ++it)
        it->box->setVisible(false);

    QHash<QString, QHash<QString, int>> counts; // namespace -> tag -> count
    for (const Post &p : t->localPosts) {
        for (auto it = p.customTags.begin(); it != p.customTags.end(); ++it) {
            for (const QString &tag : it.value()) {
                if (!tag.trimmed().isEmpty())
                    counts[it.key()][tag]++;
            }
        }
        for (const QString &tag :
             p.tagsGeneral.split(QChar::Space, Qt::SkipEmptyParts))
            counts[QString()][tag]++;
    }

    // General first, then the usual PTR namespaces, then anything else
    // alphabetically (furry/weird namespaces must never disappear).
    const QStringList preferred = {
        QString(),          QStringLiteral("creator"),
        QStringLiteral("artist"),    QStringLiteral("character"),
        QStringLiteral("series"),    QStringLiteral("copyright"),
        QStringLiteral("studio"),    QStringLiteral("species"),
        QStringLiteral("rating"),    QStringLiteral("meta"),
        QStringLiteral("title"),     QStringLiteral("source"),
        QStringLiteral("filename"),  QStringLiteral("medium"),
        QStringLiteral("style"),
    };
    QStringList order;
    for (const QString &ns : preferred) {
        if (counts.contains(ns))
            order << ns;
    }
    QStringList rest = counts.keys();
    std::sort(rest.begin(), rest.end(), [](const QString &a, const QString &b) {
        return a.compare(b, Qt::CaseInsensitive) < 0;
    });
    for (const QString &ns : rest) {
        if (!order.contains(ns))
            order << ns;
    }

    auto top = [](const QHash<QString, int> &m) {
        QVector<QPair<QString, int>> v;
        v.reserve(m.size());
        for (auto it = m.begin(); it != m.end(); ++it)
            v.append({it.key(), it.value()});
        std::sort(v.begin(), v.end(),
                  [](const auto &a, const auto &b) {
                      if (a.second != b.second)
                          return a.second > b.second;
                      return a.first < b.first;
                  });
        if (v.size() > 30)
            v.resize(30);
        return v;
    };

    for (const QString &ns : order) {
        const QVector<QPair<QString, int>> entries = top(counts.value(ns));
        auto sec = m_customSections.find(ns);
        if (sec == m_customSections.end()) {
            auto *box = new QWidget(this);
            auto *lay = new QVBoxLayout(box);
            lay->setContentsMargins(0, 0, 0, 0);
            lay->setSpacing(2);
            auto *title = new QLabel(this);
            title->setStyleSheet(QStringLiteral(
                "color:#4a5068;font-size:10px;font-weight:bold;"));
            lay->addWidget(title);
            auto *list = new QListWidget(this);
            list->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
            list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
            lay->addWidget(list, 1);
            box->setMinimumHeight(52);
            m_sideSplit->addWidget(box);
            m_sideSplit->setStretchFactor(m_sideSplit->count() - 1, 1);
            connect(list, &QListWidget::itemClicked, this,
                    &MainWindow::onTagClicked);
            list->setContextMenuPolicy(Qt::CustomContextMenu);
            connect(list, &QListWidget::customContextMenuRequested, this,
                    &MainWindow::onTagContextMenu);
            sec = m_customSections.insert(ns, {box, title, list});
        }
        sec->title->setText(namespaceTitle(ns));
        sec->list->clear();
        const QColor color = UiIcons::tagBucketColor(namespaceBucket(ns));
        for (const auto &[tag, n] : entries) {
            const QString token =
                ns.isEmpty() ? tag : ns + u':' + tag;
            QString shown = tag;
            shown.replace(u'_', u' ');
            auto *item = new QListWidgetItem(
                QStringLiteral("%1  (%2)").arg(shown).arg(n));
            item->setData(Qt::UserRole, token);
            item->setForeground(color);
            if (m_activeTags.contains(token)) {
                QFont f = item->font();
                f.setBold(true);
                item->setFont(f);
            }
            sec->list->addItem(item);
        }
        sec->box->setVisible(!entries.isEmpty());
        const int n = static_cast<int>(qMin<qsizetype>(entries.size(), 30));
        sec->box->setMaximumHeight(
            n <= 3 && n > 0 ? 18 + n * 22 + 6 : QWIDGETSIZE_MAX);
    }
    for (auto it = m_customSections.begin(); it != m_customSections.end(); ++it) {
        if (!order.contains(it.key()))
            it->box->setVisible(false);
    }
    applySidebarScrollMode();
}

void MainWindow::setHydrusMissingOnly(bool on) {
    BrowserTab *t = currentTab();
    if (!t || !t->isHydrus)
        return;
    t->hydrusOnlyMissing = on;
    if (m_hydrusMissingBtn) {
        m_hydrusMissingBtn->blockSignals(true);
        m_hydrusMissingBtn->setChecked(on);
        m_hydrusMissingBtn->blockSignals(false);
    }
    applyHydrusFilter();
}

void MainWindow::showHydrusImportMenu() {
    if (!m_hydrusImportBtn)
        return;
    QMenu menu(this);
    QAction *files = menu.addAction(tr("Add files…"));
    connect(files, &QAction::triggered, this, &MainWindow::importHydrusFiles);
    QAction *folder = menu.addAction(tr("Add folder…"));
    connect(folder, &QAction::triggered, this, &MainWindow::importHydrusFolder);
    QAction *urls = menu.addAction(tr("Add from URL(s)…"));
    connect(urls, &QAction::triggered, this, &MainWindow::importHydrusUrls);
    menu.exec(m_hydrusImportBtn->mapToGlobal(
        QPoint(0, m_hydrusImportBtn->height())));
}

void MainWindow::importHydrusFiles() {
    QStringList filters;
    for (const QString &e : hydrusMediaExtensions())
        filters << QStringLiteral("*.") + e;
    const QStringList paths = QFileDialog::getOpenFileNames(
        this, tr("Add files to Hydrus"), QDir::homePath(),
        tr("Media (%1);;All files (*)").arg(filters.join(u' ')));
    if (!paths.isEmpty())
        importHydrusPaths(paths, QString());
}

void MainWindow::importHydrusFolder() {
    const QString dir = QFileDialog::getExistingDirectory(
        this, tr("Add a folder to Hydrus"), QDir::homePath());
    if (!dir.isEmpty())
        importHydrusPaths({dir}, QString());
}

void MainWindow::importHydrusUrls() {
    if (m_hydrusDlRunner || m_hydrusStageWatcher || m_hydrusLookupActive) {
        statusBar()->showMessage(
            tr("A Hydrus import or tag refresh is already running."), 4000);
        return;
    }
    if (!GalleryDl::available()) {
        QMessageBox::warning(
            this, tr("Hydrus"),
            tr("gallery-dl is not installed or not in PATH."));
        return;
    }
    bool ok = false;
    const QString text = QInputDialog::getMultiLineText(
        this, tr("Add from URL(s)"),
        tr("Paste gallery or post URLs, one per line:"), QString(), &ok);
    if (!ok)
        return;
    QStringList urls;
    for (const QString &line : text.split(u'\n')) {
        const QString url = line.trimmed();
        if (!url.isEmpty())
            urls << url;
    }
    if (urls.isEmpty())
        return;
    // A private staging folder; gallery-dl's postprocessors are disabled
    // and the media is copied out, then the whole folder is deleted.
    const QString dir =
        QStandardPaths::writableLocation(QStandardPaths::TempLocation) +
        QStringLiteral("/box-hydrus-") +
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (!QDir().mkpath(dir)) {
        QMessageBox::warning(this, tr("Hydrus"),
                             tr("Cannot create a temporary import folder."));
        return;
    }
    m_hydrusDlUrls = urls;
    m_hydrusDlDir = dir;
    m_hydrusDlFirst.clear();
    m_hydrusDlCancelled = false;
    if (m_hydrusDlDialog)
        m_hydrusDlDialog->deleteLater();
    m_hydrusDlDialog = new GalleryDlDialog(dir, this);
    m_hydrusDlDialog->setShowFolderActions(false);
    m_hydrusDlDialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(m_hydrusDlDialog, &QObject::destroyed, this,
            [this] { m_hydrusDlDialog = nullptr; });
    connect(m_hydrusDlDialog, &GalleryDlDialog::cancelRequested, this, [this] {
        m_hydrusDlCancelled = true;
        m_hydrusDlUrls.clear();
        if (m_hydrusDlRunner)
            m_hydrusDlRunner->cancel();
        finishHydrusGallery();
    });
    connect(m_hydrusDlDialog, &QDialog::finished, this, [this] {
        // Closing the progress window mid-flight means "stop", not "keep
        // downloading out of sight".
        if (m_hydrusDlRunner) {
            m_hydrusDlCancelled = true;
            m_hydrusDlUrls.clear();
            m_hydrusDlRunner->cancel();
        }
    });
    m_hydrusDlDialog->show();
    startNextHydrusGallery();
}

void MainWindow::startNextHydrusGallery() {
    if (m_hydrusDlCancelled || m_hydrusDlUrls.isEmpty()) {
        finishHydrusGallery();
        return;
    }
    if (m_hydrusDlRunner || m_hydrusDlDir.isEmpty())
        return;
    const QString url = m_hydrusDlUrls.takeFirst();
    if (m_hydrusDlFirst.isEmpty())
        m_hydrusDlFirst = url;
    if (m_hydrusDlDialog)
        m_hydrusDlDialog->showCurrent(url);
    auto *runner = new GalleryDlRunner(GalleryDl::findBinary(), m_hydrusDlDir,
                                       url, this);
    m_hydrusDlRunner = runner;
    connect(runner, &GalleryDlRunner::logLine, this, [this](const QString &line) {
        if (m_hydrusDlDialog)
            m_hydrusDlDialog->appendLog(line);
    });
    connect(runner, &GalleryDlRunner::progress, this,
            [this](const QString &current) {
                if (m_hydrusDlDialog)
                    m_hydrusDlDialog->showCurrent(current);
            });
    connect(runner, &GalleryDlRunner::finished, this,
            [this](const GalleryDlRunner::Summary &summary) {
                if (m_hydrusDlRunner) {
                    m_hydrusDlRunner->deleteLater();
                    m_hydrusDlRunner = nullptr;
                }
                if (!summary.ok && m_hydrusDlDialog) {
                    m_hydrusDlDialog->appendLog(
                        tr("Failed: %1")
                            .arg(summary.error.isEmpty()
                                     ? tr("unknown gallery-dl error")
                                     : summary.error));
                }
                startNextHydrusGallery();
            });
    runner->start();
}

void MainWindow::finishHydrusGallery() {
    if (m_hydrusDlDir.isEmpty())
        return; // already finished (cancel then runner-finished both land here)
    const QString dir = m_hydrusDlDir;
    const QString source = m_hydrusDlFirst;
    const QStringList files = m_hydrusDlCancelled ? QStringList()
                                                  : mediaUnder(dir);
    if (m_hydrusDlDialog) {
        m_hydrusDlDialog->showFinished(!m_hydrusDlCancelled, files.size(),
                                       QString());
        m_hydrusDlDialog->showCurrent(
            m_hydrusDlCancelled
                ? tr("Cancelled.")
                : tr("Done: %1 file(s) ready to import.").arg(files.size()));
    }
    m_hydrusDlUrls.clear();
    m_hydrusDlFirst.clear();
    m_hydrusDlDir.clear();
    m_hydrusImportCleanupDir = dir;
    if (!files.isEmpty())
        importHydrusPaths(files, source);
    else if (!dir.isEmpty() && QDir(dir).exists())
        QDir(dir).removeRecursively();
}

void MainWindow::importHydrusPaths(const QStringList &paths,
                                   const QString &source) {
    if (!ensureHydrusDb())
        return;
    if (m_hydrusStageWatcher || m_hydrusLookupActive) {
        statusBar()->showMessage(
            tr("A Hydrus import or tag refresh is already running."), 4000);
        return;
    }
    QStringList files = collectMediaFiles(paths);
    // Hydrus can store anything, but the app can only show its media
    // formats. Pixiv ugoira zips are converted like downloads; other
    // archives/layered files are reported as skipped.
    QStringList converted;
    QStringList convertErrors;
    const QStringList zips = [&files] {
        QStringList z;
        for (const QString &p : files) {
            if (QFileInfo(p).suffix().compare(QStringLiteral("zip"),
                                              Qt::CaseInsensitive) == 0)
                z << p;
        }
        return z;
    }();
    if (!zips.isEmpty()) {
        const QString tmp = QStandardPaths::writableLocation(
                                QStandardPaths::TempLocation) +
                            QStringLiteral("/box-hydrus-ugoira-") +
                            QUuid::createUuid().toString(QUuid::WithoutBraces);
        QDir().mkpath(tmp);
        QString format = m_settings.ugoiraFormat;
        if (format != QStringLiteral("jxl"))
            format = QStringLiteral("webp"); // never import a zip
        QProgressDialog progress(tr("Converting ugoira…"), QString(), 0, 0,
                                 this);
        progress.setWindowModality(Qt::WindowModal);
        progress.setCancelButton(nullptr);
        progress.show();
        int n = 0;
        for (const QString &zip : zips) {
            files.removeAll(zip);
            if (!Ugoira::looksLikeUgoiraZip(zip)) {
                convertErrors << tr("%1 is an archive, not media")
                                     .arg(QFileInfo(zip).fileName());
                continue;
            }
            Ugoira::Frames frames;
            QString err;
            if (!Ugoira::unzipFrames(zip, frames, {},
                                     m_settings.ugoiraFrameDelayMs, &err)) {
                convertErrors << tr("%1: %2")
                                     .arg(QFileInfo(zip).fileName(), err);
                continue;
            }
            const QString out =
                tmp + QStringLiteral("/ugoira-%1.%2").arg(++n).arg(format);
            if (!Ugoira::encodeAnimated(frames, format, out, nullptr, &err)) {
                convertErrors << tr("%1: %2")
                                     .arg(QFileInfo(zip).fileName(), err);
                continue;
            }
            converted << out;
            QCoreApplication::processEvents();
        }
        files += converted;
        // Reuse the cleanup hook after staging has copied the results.
        if (m_hydrusImportCleanupDir.isEmpty())
            m_hydrusImportCleanupDir = tmp;
        else
            m_hydrusImportCleanupDir += QLatin1Char('\n') + tmp;
    }
    files.removeDuplicates();
    if (files.isEmpty()) {
        QString msg = tr("No importable media found.");
        if (!convertErrors.isEmpty())
            msg += u'\n' + convertErrors.join(u'\n');
        QMessageBox::information(this, tr("Hydrus"), msg);
        if (!m_hydrusImportCleanupDir.isEmpty()) {
            for (const QString &d : m_hydrusImportCleanupDir.split(u'\n')) {
                if (!d.isEmpty())
                    QDir(d).removeRecursively();
            }
            m_hydrusImportCleanupDir.clear();
        }
        return;
    }

    QSet<QString> known;
    for (const HydrusStore::Record &r : m_hydrusDb.list())
        known.insert(r.sha256);
    m_hydrusImportNew = 0;
    m_hydrusImportDup = 0;
    m_hydrusImportFail = 0;

    m_hydrusProgress = new QProgressDialog(
        tr("Preparing Hydrus import…"), QString(), 0, 0, this);
    m_hydrusProgress->setWindowModality(Qt::WindowModal);
    m_hydrusProgress->setCancelButton(nullptr);
    m_hydrusProgress->setMinimumDuration(0);
    m_hydrusProgress->show();

    auto *watcher = new QFutureWatcher<QVector<HydrusStore::StagedFile>>(this);
    m_hydrusStageWatcher = watcher;
    const QString dest = effectiveHydrusDir(m_settings);
    connect(watcher,
            &QFutureWatcher<QVector<HydrusStore::StagedFile>>::finished, this,
            [this, watcher, dest, source, convertErrors] {
                QVector<HydrusStore::StagedFile> staged = watcher->result();
                watcher->deleteLater();
                if (m_hydrusStageWatcher == watcher)
                    m_hydrusStageWatcher = nullptr;
                if (m_hydrusProgress) {
                    m_hydrusProgress->close();
                    m_hydrusProgress->deleteLater();
                    m_hydrusProgress = nullptr;
                }
                QList<HydrusLookupJob> jobs;
                const qint64 now = QDateTime::currentMSecsSinceEpoch();
                for (const HydrusStore::StagedFile &s : staged) {
                    if (!s.error.isEmpty()) {
                        ++m_hydrusImportFail;
                        continue;
                    }
                    if (s.duplicate) {
                        ++m_hydrusImportDup;
                        continue;
                    }
                    HydrusStore::Record r;
                    r.sha256 = s.sha256;
                    r.fileName = QFileInfo(s.target).fileName();
                    r.ext = s.ext;
                    r.bytes = QFileInfo(s.target).size();
                    r.addedMs = now;
                    r.source = source;
                    if (!m_hydrusDb.insert(r)) {
                        ++m_hydrusImportDup;
                        continue;
                    }
                    ++m_hydrusImportNew;
                    m_hydrusMissing.insert(r.sha256);
                    jobs.append({r.sha256, true, false, false});
                }
                if (BrowserTab *t = currentTab(); t && t->isHydrus)
                    loadHydrusTab(t);
                if (!convertErrors.isEmpty())
                    statusBar()->showMessage(convertErrors.first(), 8000);
                QString msg = tr("Imported %1 file(s)").arg(m_hydrusImportNew);
                if (m_hydrusImportDup)
                    msg += tr(", %1 already present").arg(m_hydrusImportDup);
                if (m_hydrusImportFail)
                    msg += tr(", %1 failed").arg(m_hydrusImportFail);
                statusBar()->showMessage(msg + tr(" · querying Hydrus tags…"),
                                         10000);
                if (jobs.isEmpty()) {
                    if (!m_hydrusImportCleanupDir.isEmpty()) {
                        for (const QString &d :
                             m_hydrusImportCleanupDir.split(u'\n')) {
                            if (!d.isEmpty())
                                QDir(d).removeRecursively();
                        }
                        m_hydrusImportCleanupDir.clear();
                    }
                    maybeScanHydrusOrphans(true);
                    return;
                }
                startHydrusLookups(jobs);
            });
    watcher->setFuture(QtConcurrent::run(&stageHydrusFiles, files, dest, known));
}

HydrusStore::Record MainWindow::hydrusRecordFor(const Post &p) const {
    if (p.sha256.isEmpty() || !m_hydrusDb.isOpen())
        return {};
    return m_hydrusDb.record(p.sha256);
}

void MainWindow::showHydrusSuggestions(const QString &prefix) {
    BrowserTab *t = currentTab();
    if (!t || !t->isHydrus)
        return;
    const QString p = prefix.trimmed().toLower();
    QHash<QString, int> counts;
    QHash<QString, QString> buckets;
    auto add = [&](const QString &token, const QString &bucket) {
        if (token.isEmpty() ||
            (!p.isEmpty() && !token.toLower().startsWith(p)))
            return;
        counts[token]++;
        if (!buckets.contains(token))
            buckets.insert(token, bucket);
    };
    for (const Post &post : t->localPosts) {
        for (auto it = post.customTags.begin(); it != post.customTags.end();
             ++it) {
            const QString bucket = namespaceBucket(it.key());
            for (const QString &tag : it.value()) {
                add(it.key() + u':' + tag, bucket);
                add(tag, bucket);
            }
        }
        for (const QString &tag :
             post.tagsGeneral.split(QChar::Space, Qt::SkipEmptyParts))
            add(tag, QStringLiteral("general"));
    }
    struct Entry {
        QString tag;
        QString bucket;
        int count = 0;
    };
    QVector<Entry> entries;
    entries.reserve(counts.size());
    for (auto it = counts.begin(); it != counts.end(); ++it)
        entries.append({it.key(), buckets.value(it.key()), it.value()});
    std::sort(entries.begin(), entries.end(),
              [](const Entry &a, const Entry &b) {
                  if (a.count != b.count)
                      return a.count > b.count;
                  if (a.tag.size() != b.tag.size())
                      return a.tag.size() < b.tag.size();
                  return a.tag.compare(b.tag, Qt::CaseInsensitive) < 0;
              });
    if (entries.size() > 30)
        entries.resize(30);
    QStringList words, bs;
    QVector<int> nums;
    for (const Entry &e : entries) {
        words << e.tag;
        bs << e.bucket;
        nums << e.count;
    }
    fillSuggestModel(words, bs, nums);
    if (!words.isEmpty() && m_completer && m_searchEdit &&
        m_searchEdit->isVisible()) {
        m_completer->setCompletionPrefix(prefix);
        m_completer->complete();
    }
}

void MainWindow::startHydrusLookups(const QList<HydrusLookupJob> &jobs) {
    if (jobs.isEmpty() || m_hydrusLookupActive)
        return;
    m_hydrusLookupQueue = jobs;
    m_hydrusLookupDone = 0;
    m_hydrusLookupTotal = static_cast<int>(jobs.size());
    m_hydrusLookupMissing = 0;
    m_hydrusLookupManual = 0;
    m_hydrusLookupErrors = 0;
    m_hydrusLookupSkipped = 0;
    m_hydrusMaskedTags = 0;
    m_hydrusCountsSkipBatch =
        m_hydrusCountsUnavailable && m_settings.hydrusMinTagPosts > 0;
    m_hydrusCountsWarned = false;
    m_hydrusAwaitingCounts = false;
    m_hydrusLookupActive = true;
    m_hydrusLookupRefresh =
        std::all_of(jobs.begin(), jobs.end(),
                    [](const HydrusLookupJob &j) { return j.refresh; });
    m_hydrusLookupClient =
        new Hydrus::Client(hydrusConfigFromSettings(m_settings), this);

    auto *client = m_hydrusLookupClient;
    connect(client, &Hydrus::Client::found, this,
            [this](const Hydrus::FileTags &file, Hydrus::HashKind) {
                m_hydrusCurrentFileId = file.fileId;
                m_hydrusCurrentRawTags =
                    HydrusStore::normalizeTags(file.tags);
                // Masking is a hydd extension. Without it, or after a
                // failure in this batch, keep every fetched tag and let
                // the summary/status explain why.
                if (m_settings.hydrusMinTagPosts > 0 &&
                    !m_hydrusCountsUnavailable && !m_hydrusCountsSkipBatch &&
                    !m_hydrusCurrentRawTags.isEmpty()) {
                    m_hydrusAwaitingCounts = true;
                    m_hydrusLookupClient->tagCounts(m_hydrusCurrentRawTags);
                    return;
                }
                applyHydrusFetched(m_hydrusCurrentRawTags);
            });
    connect(client, &Hydrus::Client::tagCountsReady, this,
            [this](const QHash<QString, int> &counts,
                   const QStringList &unknown) {
                if (!m_hydrusAwaitingCounts)
                    return;
                m_hydrusAwaitingCounts = false;
                HydrusStore::TagMask mask;
                mask.minPosts = m_settings.hydrusMinTagPosts;
                mask.dropUnknown = m_settings.hydrusDropUnknownTags;
                const QStringList kept = HydrusStore::maskTagsByCounts(
                    m_hydrusCurrentRawTags, counts, unknown, mask);
                m_hydrusMaskedTags +=
                    m_hydrusCurrentRawTags.size() - kept.size();
                applyHydrusFetched(kept);
            });
    connect(client, &Hydrus::Client::tagCountsFailed, this,
            [this](const QString &message, int status) {
                if (!m_hydrusAwaitingCounts)
                    return;
                m_hydrusAwaitingCounts = false;
                m_hydrusCountsSkipBatch = true;
                if (status == 404 || status == 501)
                    m_hydrusCountsUnavailable = true;
                if (!m_hydrusCountsWarned) {
                    m_hydrusCountsWarned = true;
                    const QString why =
                        status == 404 || status == 501
                            ? tr("hydd /hyd/tag_counts is disabled; reload "
                                 "to retry")
                            : status == 503
                                  ? tr("hydd is still building tag counts")
                                  : message;
                    m_hydrusCountsWhy = why;
                    statusBar()->showMessage(
                        tr("Hydrus tag counts unavailable (%1); tags are "
                           "kept unmasked.").arg(why),
                        12000);
                }
                applyHydrusFetched(m_hydrusCurrentRawTags);
            });
    connect(client, &Hydrus::Client::notFound, this, [this](Hydrus::HashKind) {
        const HydrusLookupJob job = m_hydrusCurrentJob;
        if (job.promptMissing) {
            const HydrusStore::Record rec = m_hydrusDb.record(job.sha256);
            HydrusTagDialog dlg(
                rec.fileName,
                tr("Hydrus does not know this file (yet). Add tags now? "
                   "You can also do this later from the post's menu."),
                rec.tags, this);
            if (dlg.exec() == QDialog::Accepted) {
                m_hydrusDb.updateManualTags(job.sha256, dlg.tags());
                ++m_hydrusLookupManual;
            }
        }
        m_hydrusDb.markMissed(job.sha256);
        m_hydrusMissing.insert(job.sha256);
        ++m_hydrusLookupMissing;
        nextHydrusLookup();
    });
    connect(client, &Hydrus::Client::error, this, [this](const QString &error) {
        ++m_hydrusLookupErrors;
        m_hydrusDb.markMissed(m_hydrusCurrentJob.sha256);
        m_hydrusMissing.insert(m_hydrusCurrentJob.sha256);
        m_hydrusLookupQueue.clear();
        statusBar()->showMessage(tr("Hydrus lookup failed: %1").arg(error),
                                 10000);
        finishHydrusLookups();
    });
    nextHydrusLookup();
}

void MainWindow::nextHydrusLookup() {
    if (!m_hydrusLookupActive)
        return;
    if (m_hydrusLookupQueue.isEmpty()) {
        finishHydrusLookups();
        return;
    }
    m_hydrusCurrentJob = m_hydrusLookupQueue.takeFirst();
    m_hydrusAwaitingCounts = false;
    m_hydrusCurrentRawTags.clear();
    m_hydrusCurrentFileId = 0;
    ++m_hydrusLookupDone;
    statusBar()->showMessage(
        tr("Querying Hydrus tags… %1 / %2")
            .arg(m_hydrusLookupDone)
            .arg(m_hydrusLookupTotal),
        0);
    m_hydrusLookupClient->lookup(m_hydrusCurrentJob.sha256,
                                 Hydrus::HashKind::Sha256);
}

void MainWindow::applyHydrusFetched(const QStringList &tags) {
    const HydrusLookupJob job = m_hydrusCurrentJob;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    bool replace = true;
    if (job.refresh) {
        const HydrusStore::Record rec = m_hydrusDb.record(job.sha256);
        if (job.force && rec.manualOverride) {
            HydrusRefreshPreviewDialog dlg(rec.fileName, true, rec.tags,
                                           tags, this);
            dlg.exec();
            replace = dlg.choice() ==
                      HydrusRefreshPreviewDialog::Choice::Replace;
        }
        m_hydrusDb.updateTags(
            job.sha256, replace ? tags : rec.tags, true,
            m_hydrusCurrentFileId, now,
            replace && job.force && rec.manualOverride);
    } else {
        m_hydrusDb.updateTags(job.sha256, tags, true,
                              m_hydrusCurrentFileId, now);
    }
    m_hydrusMissing.remove(job.sha256);
    m_hydrusAwaitingCounts = false;
    nextHydrusLookup();
}

void MainWindow::finishHydrusLookups() {
    if (!m_hydrusLookupActive && !m_hydrusLookupClient)
        return;
    m_hydrusLookupActive = false;
    if (m_hydrusLookupClient) {
        m_hydrusLookupClient->cancel();
        m_hydrusLookupClient->deleteLater();
        m_hydrusLookupClient = nullptr;
    }
    m_hydrusLookupQueue.clear();
    const bool refresh = m_hydrusLookupRefresh;
    const int missing = m_hydrusLookupMissing;
    const int manual = m_hydrusLookupManual;
    const int errors = m_hydrusLookupErrors;
    const int skipped = m_hydrusLookupSkipped;
    m_hydrusLookupRefresh = false;
    if (BrowserTab *t = currentTab(); t && t->isHydrus)
        loadHydrusTab(t);
    QString msg;
    if (refresh) {
        msg = tr("Refreshed Hydrus tags for %1 post(s)")
                  .arg(m_hydrusLookupDone - missing - errors);
        if (skipped)
            msg += tr(", %1 manual edit(s) skipped").arg(skipped);
        if (missing)
            msg += tr(", %1 still not on Hydrus").arg(missing);
    } else {
        msg = tr("Imported %1 file(s)").arg(m_hydrusImportNew);
        if (m_hydrusImportDup)
            msg += tr(", %1 already present").arg(m_hydrusImportDup);
        if (m_hydrusImportFail)
            msg += tr(", %1 failed").arg(m_hydrusImportFail);
        if (manual)
            msg += tr(", %1 tagged by hand").arg(manual);
        if (missing - manual > 0)
            msg += tr(", %1 without Hydrus tags").arg(missing - manual);
    }
    if (m_hydrusMaskedTags > 0)
        msg += tr(", %1 masked tag(s)").arg(m_hydrusMaskedTags);
    if (m_settings.hydrusMinTagPosts > 0 && m_hydrusCountsSkipBatch)
        msg += tr(", tag masking skipped (%1)")
                   .arg(m_hydrusCountsWhy.isEmpty()
                            ? tr("counts unavailable")
                            : m_hydrusCountsWhy);
    if (errors)
        msg += tr(", %1 lookup error(s)").arg(errors);
    statusBar()->showMessage(msg, 12000);
    if (!m_hydrusImportCleanupDir.isEmpty()) {
        for (const QString &d : m_hydrusImportCleanupDir.split(u'\n')) {
            if (!d.isEmpty())
                QDir(d).removeRecursively();
        }
        m_hydrusImportCleanupDir.clear();
    }
    maybeScanHydrusOrphans(true);
}

void MainWindow::maybeScanHydrusOrphans(bool force) {
    if (m_hydrusOrphanWatcher)
        return;
    if (m_hydrusOrphanScanDone && !force)
        return;
    if (!ensureHydrusDb())
        return;
    const QString dir = effectiveHydrusDir(m_settings);
    if (!QDir(dir).exists())
        return;
    QSet<QString> known;
    for (const HydrusStore::Record &r : m_hydrusDb.list())
        known.insert(r.sha256);
    auto *watcher =
        new QFutureWatcher<QVector<HydrusStore::Record>>(this);
    m_hydrusOrphanWatcher = watcher;
    connect(watcher, &QFutureWatcher<QVector<HydrusStore::Record>>::finished,
            this, [this, watcher] {
                const QVector<HydrusStore::Record> found = watcher->result();
                watcher->deleteLater();
                if (m_hydrusOrphanWatcher == watcher)
                    m_hydrusOrphanWatcher = nullptr;
                m_hydrusOrphanScanDone = true;
                int added = 0;
                for (const HydrusStore::Record &r : found) {
                    if (m_hydrusDb.insert(r))
                        ++added;
                }
                if (added > 0) {
                    statusBar()->showMessage(
                        tr("Indexed %1 existing Hydrus file(s)").arg(added),
                        5000);
                    if (BrowserTab *t = currentTab(); t && t->isHydrus)
                        loadHydrusTab(t);
                }
            });
    watcher->setFuture(
        QtConcurrent::run(&scanHydrusOrphans, dir, known));
}

void MainWindow::editHydrusTags(const QVector<Post> &posts) {
    if (posts.isEmpty() || !ensureHydrusDb())
        return;
    if (posts.size() > 1) {
        if (QMessageBox::question(
                this, tr("Edit Hydrus tags"),
                tr("Edit tags for %1 posts? One editor opens per post.")
                    .arg(posts.size())) != QMessageBox::Yes)
            return;
    }
    int changed = 0;
    for (const Post &p : posts) {
        HydrusStore::Record r = hydrusRecordFor(p);
        if (r.sha256.isEmpty())
            continue;
        HydrusTagDialog dlg(
            r.fileName,
            r.manualOverride
                ? tr("These tags were edited by hand. Saving keeps them "
                     "protected from automatic refreshes.")
                : QString(),
            r.tags, this);
        if (dlg.exec() != QDialog::Accepted)
            continue;
        if (m_hydrusDb.updateManualTags(r.sha256, dlg.tags())) {
            ++changed;
            // A manual tag set is user-owned content, but it is still a
            // row the server has never seen: leave m_hydrusMissing alone
            // so "Not on Hydrus" keeps finding it (manual tags or not).
        }
    }
    if (changed > 0) {
        statusBar()->showMessage(
            tr("Saved Hydrus tags for %1 post(s)").arg(changed), 5000);
        if (BrowserTab *t = currentTab(); t && t->isHydrus)
            loadHydrusTab(t);
    }
}

void MainWindow::refreshHydrusTags(const QVector<Post> &posts, bool force) {
    if (posts.isEmpty() || !ensureHydrusDb())
        return;
    if (m_hydrusLookupActive) {
        statusBar()->showMessage(tr("A Hydrus lookup is already running."),
                                 4000);
        return;
    }
    QList<HydrusLookupJob> jobs;
    int overrides = 0;
    int skipped = 0;
    for (const Post &p : posts) {
        const HydrusStore::Record r = hydrusRecordFor(p);
        if (r.sha256.isEmpty())
            continue;
        if (r.manualOverride && !force) {
            ++skipped;
            continue;
        }
        if (r.manualOverride)
            ++overrides;
        jobs.append({r.sha256, false, true, force});
    }
    if (jobs.isEmpty()) {
        statusBar()->showMessage(
            skipped ? tr("Nothing to refresh: %1 manually edited post(s) "
                        "were skipped (use Force refresh to include them).")
                          .arg(skipped)
                    : tr("Nothing to refresh."),
            7000);
        return;
    }
    if (force && overrides > 0) {
        if (QMessageBox::question(
                this, tr("Force refresh Hydrus tags"),
                tr("%1 manually edited post(s) are included. Each one shows "
                   "its current and refreshed tags before anything is "
                   "replaced.\n\nContinue?")
                    .arg(overrides)) != QMessageBox::Yes)
            return;
    }
    startHydrusLookups(jobs);
    m_hydrusLookupSkipped = skipped;
}

void MainWindow::showHydrusGridMenu(const QPoint &pos) {
    const QModelIndex clicked = m_grid->indexAt(pos);
    if (!clicked.isValid() || clicked.row() < 0 ||
        clicked.row() >= m_current.size())
        return;
    const QModelIndexList sel = m_grid->selectionModel()->selectedRows();
    bool clickSelected = false;
    for (const QModelIndex &i : sel) {
        if (i.row() == clicked.row()) {
            clickSelected = true;
            break;
        }
    }
    const QModelIndexList rows = clickSelected ? sel : QModelIndexList{clicked};
    QVector<Post> targets;
    for (const QModelIndex &i : rows) {
        if (i.row() >= 0 && i.row() < m_current.size())
            targets.append(m_current.at(i.row()));
    }
    if (targets.isEmpty())
        return;

    QMenu menu(this);
    QAction *edit = menu.addAction(tr("Edit Hydrus tags…"));
    connect(edit, &QAction::triggered, this,
            [this, targets] { editHydrusTags(targets); });
    menu.addSeparator();
    QAction *refresh = menu.addAction(tr("Refresh Hydrus tags"));
    connect(refresh, &QAction::triggered, this, [this, targets] {
        refreshHydrusTags(targets, false);
    });
    QAction *forceRefresh = menu.addAction(tr("Force refresh Hydrus tags…"));
    connect(forceRefresh, &QAction::triggered, this, [this, targets] {
        refreshHydrusTags(targets, true);
    });
    menu.addSeparator();
    if (targets.size() == 1) {
        const Post &p = targets.first();
        if (!p.absFile.isEmpty() && QFileInfo::exists(p.absFile)) {
            QAction *show = menu.addAction(tr("Show in folder"));
            connect(show, &QAction::triggered, this, [path = p.absFile] {
                DesktopFiles::showInFolder(path);
            });
            QAction *copyPath = menu.addAction(tr("Copy file path"));
            connect(copyPath, &QAction::triggered, this, [path = p.absFile] {
                QApplication::clipboard()->setText(path);
            });
            QAction *open = menu.addAction(tr("Open file"));
            connect(open, &QAction::triggered, this, [path = p.absFile] {
                QDesktopServices::openUrl(QUrl::fromLocalFile(path));
            });
        }
    }
    QAction *copy = menu.addAction(tr("Copy file (clipboard)"));
    connect(copy, &QAction::triggered, this, [this, targets] {
        copyPostFilesToClipboard(targets);
    });
    menu.exec(m_grid->viewport()->mapToGlobal(pos));
}

void MainWindow::showHydrusViewerMenu(const Post &post,
                                      const QPoint &globalPos) {
    QMenu menu(this);
    QAction *full = menu.addAction(
        m_viewerFullscreen ? tr("Exit fullscreen") : tr("Fullscreen"));
    full->setShortcut(QKeySequence(Qt::Key_F11));
    connect(full, &QAction::triggered, this,
            &MainWindow::toggleViewerFullscreen);
    menu.addSeparator();
    QAction *edit = menu.addAction(tr("Edit Hydrus tags…"));
    connect(edit, &QAction::triggered, this,
            [this, post] { editHydrusTags({post}); });
    QAction *refresh = menu.addAction(tr("Refresh Hydrus tags"));
    connect(refresh, &QAction::triggered, this,
            [this, post] { refreshHydrusTags({post}, false); });
    QAction *forceRefresh = menu.addAction(tr("Force refresh Hydrus tags…"));
    connect(forceRefresh, &QAction::triggered, this,
            [this, post] { refreshHydrusTags({post}, true); });
    menu.addSeparator();
    if (!post.absFile.isEmpty() && QFileInfo::exists(post.absFile)) {
        QAction *show = menu.addAction(tr("Show in folder"));
        connect(show, &QAction::triggered, this, [path = post.absFile] {
            DesktopFiles::showInFolder(path);
        });
        QAction *copyPath = menu.addAction(tr("Copy file path"));
        connect(copyPath, &QAction::triggered, this, [path = post.absFile] {
            QApplication::clipboard()->setText(path);
        });
        QAction *copy = menu.addAction(tr("Copy file (clipboard)"));
        connect(copy, &QAction::triggered, this, [this, post] {
            copyPostFilesToClipboard({post});
        });
        QAction *open = menu.addAction(tr("Open file"));
        connect(open, &QAction::triggered, this, [path = post.absFile] {
            QDesktopServices::openUrl(QUrl::fromLocalFile(path));
        });
    }
    menu.exec(globalPos);
}
