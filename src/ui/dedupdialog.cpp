#include "dedupdialog.h"

#include "dedupgroups.h"
#include "downloadmanager.h"
#include "hydrusstore.h"
#include "metaparser.h"
#include "uicons.h"

#include <QAbstractItemView>
#include <QBrush>
#include <QCheckBox>
#include <QCloseEvent>
#include <QColor>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QImageReader>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>
#include <QtConcurrent>

#include <algorithm>

namespace {
enum ItemRole {
    PathRole = Qt::UserRole,
    ScopeRole,
    ShaRole,
    IdRole,
    RootRole,
    KeeperRole,
    DeletableRole,
};

constexpr int kMaxSidecar = 8 * 1024 * 1024;
const QColor kKeepColor(0x43, 0xd1, 0x7c);

QString rowKey(const QString &scope, const QString &path) {
    return scope + u'\n' + path;
}
} // namespace

DedupDialog::DedupDialog(const DedupDialogSources &sources,
                         HydrusStore::Db *hydrus, QWidget *parent)
    : QDialog(parent), m_sources(sources), m_hydrus(hydrus) {
    setObjectName(QStringLiteral("dedupDialog"));
    setWindowTitle(tr("Duplicate media"));
    setWindowModality(Qt::WindowModal);
    resize(1080, 760);
    if (!m_db.open(m_sources.dbPath))
        m_sources.dbPath.clear(); // report below; scans stay disabled
    buildUi();
    reloadGroups();
}

DedupDialog::~DedupDialog() {
    if (m_task) {
        m_task->requestCancel();
        if (m_watcher)
            m_watcher->waitForFinished();
        delete m_task;
        m_task = nullptr;
    }
    m_db.close();
}

void DedupDialog::buildUi() {
    auto *root = new QVBoxLayout(this);

    auto *intro = new QLabel(
        tr("Finds copies by decoded pixels plus exact file bytes. Matching is "
           "kept inside one family (local archive, downloads/favourites, "
           "Hydrus) and one media type, so those libraries never interact. "
           "Tag differences do not matter: pick the copy with the best tags "
           "as the keeper before deleting the rest."),
        this);
    intro->setWordWrap(true);
    root->addWidget(intro);

    m_sourcesBox = new QGroupBox(tr("Scan sources"), this);
    auto *sg = new QVBoxLayout(m_sourcesBox);
    m_downloadsBox = new QCheckBox(
        tr("Downloads / downloaded favourites"), m_sourcesBox);
    m_downloadsBox->setToolTip(m_sources.downloadDir);
    m_downloadsBox->setChecked(QFileInfo(m_sources.downloadDir).isDir());
    sg->addWidget(m_downloadsBox);

    m_hydrusBox = new QCheckBox(tr("Hydrus media"), m_sourcesBox);
    m_hydrusBox->setToolTip(m_sources.hydrusDir);
    const bool hydrusReady = m_hydrus != nullptr &&
                             QFileInfo(m_sources.hydrusDir).isDir();
    m_hydrusBox->setEnabled(hydrusReady);
    m_hydrusBox->setChecked(hydrusReady);
    if (!hydrusReady)
        m_hydrusBox->setToolTip(
            tr("Hydrus is disabled or its database/media folder is missing"));
    sg->addWidget(m_hydrusBox);

    sg->addWidget(new QLabel(tr("Local archives:"), m_sourcesBox));
    m_archives = new QListWidget(m_sourcesBox);
    m_archives->setSelectionMode(QAbstractItemView::NoSelection);
    m_archives->setTextElideMode(Qt::ElideMiddle);
    m_archives->setMaximumHeight(78);
    sg->addWidget(m_archives);
    const QString current = QDir(m_sources.currentRoot).canonicalPath();
    for (const QString &dir : m_sources.archiveRoots) {
        const QString canon = QDir(dir).canonicalPath();
        if (canon.isEmpty())
            continue;
        auto *item = new QListWidgetItem(QDir::toNativeSeparators(canon),
                                         m_archives);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(canon == current ? Qt::Checked : Qt::Unchecked);
        item->setToolTip(QDir::toNativeSeparators(canon));
    }
    auto *addRow = new QHBoxLayout();
    auto *addFolder = new QPushButton(tr("Add folder…"), m_sourcesBox);
    connect(addFolder, &QPushButton::clicked, this,
            &DedupDialog::addArchiveFolder);
    addRow->addWidget(addFolder);
    addRow->addStretch(1);
    sg->addLayout(addRow);

    auto *thresholdRow = new QHBoxLayout();
    thresholdRow->addWidget(new QLabel(tr("Pixel match tolerance:"), this));
    m_threshold = new QSpinBox(this);
    m_threshold->setRange(0, 16);
    m_threshold->setValue(6);
    m_threshold->setSuffix(tr(" / 64 bits"));
    m_threshold->setToolTip(
        tr("0 = identical perceptual hashes only. Higher values catch more "
           "lossy recompressions, at the risk of grouping different images. "
           "Exact byte copies always match."));
    thresholdRow->addWidget(m_threshold);
    thresholdRow->addStretch(1);
    sg->addLayout(thresholdRow);
    root->addWidget(m_sourcesBox);

    auto *scanRow = new QHBoxLayout();
    m_scanBtn = new QPushButton(tr("Scan selected"), this);
    m_stopBtn = new QPushButton(tr("Stop"), this);
    m_stopBtn->setEnabled(false);
    m_progress = new QProgressBar(this);
    m_progress->setRange(0, 1);
    m_progress->setValue(0);
    m_progress->setTextVisible(false);
    scanRow->addWidget(m_scanBtn);
    scanRow->addWidget(m_stopBtn);
    scanRow->addWidget(m_progress, 1);
    root->addLayout(scanRow);
    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    root->addWidget(m_status);

    m_tree = new QTreeWidget(this);
    m_tree->setColumnCount(8);
    m_tree->setHeaderLabels({tr("Delete"), tr("Keep"), tr("Preview"),
                             tr("File"), tr("Size"), tr("Dimensions"),
                             tr("Tags"), tr("Family")});
    m_tree->setRootIsDecorated(true);
    m_tree->setAlternatingRowColors(true);
    m_tree->setIconSize(QSize(52, 52));
    m_tree->setUniformRowHeights(false);
    m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    auto *head = m_tree->header();
    head->setSectionResizeMode(0, QHeaderView::Fixed);
    head->resizeSection(0, 58);
    head->setSectionResizeMode(1, QHeaderView::Fixed);
    head->resizeSection(1, 44);
    head->setSectionResizeMode(2, QHeaderView::Fixed);
    head->resizeSection(2, 82);
    head->setSectionResizeMode(3, QHeaderView::Stretch);
    head->resizeSection(4, 88);
    head->resizeSection(5, 96);
    head->resizeSection(6, 76);
    head->resizeSection(7, 158);
    root->addWidget(m_tree, 1);

    m_details = new QPlainTextEdit(this);
    m_details->setReadOnly(true);
    m_details->setMaximumHeight(92);
    m_details->setPlaceholderText(
        tr("Select a copy to inspect its tags and paths"));
    root->addWidget(m_details);

    auto *buttons = new QHBoxLayout();
    m_keepBtn = new QPushButton(tr("Keep selected copy"), this);
    m_keepBtn->setEnabled(false);
    m_deleteBtn = new QPushButton(tr("Delete selected…"), this);
    m_deleteBtn->setEnabled(false);
    m_clearBtn = new QPushButton(tr("Forget cached hashes"), this);
    auto *closeBtn = new QPushButton(tr("Close"), this);
    buttons->addWidget(m_keepBtn);
    buttons->addWidget(m_deleteBtn);
    buttons->addStretch(1);
    buttons->addWidget(m_clearBtn);
    buttons->addWidget(closeBtn);
    root->addLayout(buttons);

    if (!m_sources.allowArchiveDelete) {
        auto *gate = new QLabel(
            tr("Archive copies are read-only here. Enable Preferences → "
               "Library → Allow deleting duplicate files in local archives "
               "to clean archives too."),
            this);
        gate->setWordWrap(true);
        gate->setStyleSheet(QStringLiteral("color:#e0a055;"));
        root->addWidget(gate);
    }
    if (m_db.isOpen()) {
        m_status->setText(
            tr("Cached hashes: %1. Pick sources and scan; rescans only hash "
               "new or changed files.")
                .arg(m_db.count()));
    } else {
        m_status->setText(
            tr("Cannot open the hash database: %1").arg(m_db.lastError()));
        m_scanBtn->setEnabled(false);
        m_clearBtn->setEnabled(false);
    }

    connect(m_scanBtn, &QPushButton::clicked, this, &DedupDialog::startScan);
    connect(m_stopBtn, &QPushButton::clicked, this, [this] {
        if (m_task)
            m_task->requestCancel();
        m_stopBtn->setEnabled(false);
        m_status->setText(tr("Stopping the scan…"));
    });
    connect(m_keepBtn, &QPushButton::clicked, this, &DedupDialog::keepSelected);
    connect(m_deleteBtn, &QPushButton::clicked, this,
            &DedupDialog::deleteSelected);
    connect(m_clearBtn, &QPushButton::clicked, this,
            &DedupDialog::clearDatabase);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::close);
    connect(m_tree, &QTreeWidget::currentItemChanged, this,
            [this] { showDetails(); });
    connect(m_tree, &QTreeWidget::itemChanged, this,
            [this] { updateDeleteButton(); });
    connect(m_tree, &QTreeWidget::itemDoubleClicked, this,
            [this](QTreeWidgetItem *item, int) {
                if (item && item->parent())
                    keepSelected();
            });
    connect(m_threshold, QOverload<int>::of(&QSpinBox::valueChanged), this,
            [this] {
                if (!m_scanning)
                    reloadGroups();
            });
}

void DedupDialog::addArchiveFolder() {
    const QString dir = QFileDialog::getExistingDirectory(
        this, tr("Add a local archive to scan"));
    if (dir.isEmpty())
        return;
    const QString canon = QDir(dir).canonicalPath();
    if (canon.isEmpty())
        return;
    for (int i = 0; i < m_archives->count(); ++i)
        if (QDir(m_archives->item(i)->text()).canonicalPath() == canon)
            return;
    auto *item =
        new QListWidgetItem(QDir::toNativeSeparators(canon), m_archives);
    item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
    item->setCheckState(Qt::Checked);
    item->setToolTip(QDir::toNativeSeparators(canon));
}

QStringList DedupDialog::allArchiveRoots() const {
    QStringList out;
    for (int i = 0; i < m_archives->count(); ++i) {
        const QString canon =
            QDir(m_archives->item(i)->toolTip()).canonicalPath();
        if (!canon.isEmpty() && !out.contains(canon))
            out << canon;
    }
    return out;
}

QVector<DedupSource> DedupDialog::selectedSources() const {
    QVector<DedupSource> out;
    QStringList roots;
    for (int i = 0; i < m_archives->count(); ++i) {
        const QListWidgetItem *item = m_archives->item(i);
        if (item->checkState() != Qt::Checked)
            continue;
        const QString canon = QDir(item->toolTip()).canonicalPath();
        if (canon.isEmpty() || !QDir(canon).exists())
            continue;
        roots << canon;
    }
    // A root inside another selected root is covered by the parent.
    const QString downloadDir = QDir(m_sources.downloadDir).canonicalPath();
    const QString hydrusDir = QDir(m_sources.hydrusDir).canonicalPath();
    const auto insideDir = [](const QString &path, const QString &dir) {
        return !path.isEmpty() && !dir.isEmpty() &&
               (path == dir || path.startsWith(dir + u'/'));
    };
    std::sort(roots.begin(), roots.end(),
              [](const QString &a, const QString &b) {
                  return a.size() < b.size();
              });
    QStringList kept;
    for (const QString &root : roots) {
        if (insideDir(root, downloadDir) || insideDir(root, hydrusDir))
            continue; // those files belong to their own family
        bool nested = false;
        for (const QString &parent : kept) {
            if (root == parent || root.startsWith(parent + u'/')) {
                nested = true;
                break;
            }
        }
        if (!nested)
            kept << root;
    }
    for (const QString &root : kept)
        out.append({QStringLiteral("archive"), root});
    if (m_downloadsBox->isChecked() &&
        QFileInfo(m_sources.downloadDir).isDir())
        out.append({QStringLiteral("download"),
                    QDir(m_sources.downloadDir).canonicalPath()});
    if (m_hydrusBox->isChecked() && m_hydrus &&
        QFileInfo(m_sources.hydrusDir).isDir())
        out.append({QStringLiteral("hydrus"),
                    QDir(m_sources.hydrusDir).canonicalPath()});
    return out;
}

void DedupDialog::startScan() {
    if (m_scanning || !m_db.isOpen())
        return;
    const QVector<DedupSource> sources = selectedSources();
    if (sources.isEmpty()) {
        QMessageBox::information(
            this, tr("Duplicate media"),
            tr("Select at least one source: an archive, downloads or Hydrus."));
        return;
    }
    DedupScanOptions opts;
    opts.dbPath = m_db.path();
    opts.hydrusDbPath = m_sources.hydrusDbPath;
    opts.downloadDir = m_sources.downloadDir;
    opts.hydrusDir = m_sources.hydrusDir;
    opts.sources = sources;

    m_scanning = true;
    setScanning(true);
    m_progress->setRange(0, 0);
    m_status->setText(tr("Scanning selected sources…"));

    m_task = new DedupScanTask();
    m_watcher = new QFutureWatcher<void>(this);
    connect(m_task, &DedupScanTask::progress, this,
            [this](int done, int total, const QString &scope,
                   const QString &name) {
                if (total <= 0)
                    return;
                if (m_progress->maximum() != total)
                    m_progress->setRange(0, total);
                m_progress->setValue(done);
                m_status->setText(
                    tr("%1: %2 / %3 - %4")
                        .arg(dedupScopeLabel(scope))
                        .arg(done)
                        .arg(total)
                        .arg(name));
            });
    connect(m_watcher, &QFutureWatcher<void>::finished, this,
            &DedupDialog::scanFinished);
    m_watcher->setFuture(QtConcurrent::run([task = m_task, opts] {
        task->run(opts);
    }));
}

void DedupDialog::scanFinished() {
    m_scanning = false;
    const DedupScanTask::Result result =
        m_task ? m_task->result : DedupScanTask::Result{};
    if (m_task) {
        m_task->deleteLater();
        m_task = nullptr;
    }
    if (m_watcher) {
        m_watcher->deleteLater();
        m_watcher = nullptr;
    }
    m_progress->setRange(0, 1);
    m_progress->setValue(1);
    setScanning(false);
    reloadGroups();
    if (!result.error.isEmpty()) {
        m_status->setText(tr("Scan failed: %1").arg(result.error));
        QMessageBox::warning(this, tr("Duplicate media"),
                             tr("The scan could not finish:\n%1")
                                 .arg(result.error));
    } else if (result.cancelled) {
        m_status->setText(tr("Scan cancelled; cached hashes were kept."));
    } else {
        m_status->setText(
            tr("Scanned %1 files: %2 hashed, %3 cached, %4 unreadable.")
                .arg(result.total)
                .arg(result.hashed)
                .arg(result.cached)
                .arg(result.failed));
    }
    if (m_closeWhenDone)
        close();
}

void DedupDialog::setScanning(bool scanning) {
    m_sourcesBox->setEnabled(!scanning);
    m_scanBtn->setEnabled(!scanning && m_db.isOpen());
    m_stopBtn->setEnabled(scanning);
    m_clearBtn->setEnabled(!scanning);
    m_keepBtn->setEnabled(!scanning && m_tree->currentItem() &&
                          m_tree->currentItem()->parent() != nullptr);
    updateDeleteButton();
}

void DedupDialog::reloadGroups() {
    if (!m_db.isOpen())
        return;
    m_meta.clear();
    const QVector<DedupFile> all = m_db.files();
    QVector<DedupFile> rows;
    m_rows.clear();
    for (const DedupFile &f : all) {
        if (!QFileInfo::exists(f.path))
            continue; // stale row; the next scan prunes it
        m_rows.insert(rowKey(f.scope, f.path), f);
        rows.append(f);
    }
    QVector<DedupGroup> groups =
        buildDedupGroups(rows, m_threshold->value());
    // The keeper should prefer the richer metadata: tags first, then
    // pixels, then bytes. The user can override it in the dialog.
    for (DedupGroup &group : groups) {
        std::sort(group.files.begin(), group.files.end(),
                  [this](const DedupFile &a, const DedupFile &b) {
                      const Meta ma = metaFor(a);
                      const Meta mb = metaFor(b);
                      if (ma.tags != mb.tags)
                          return ma.tags > mb.tags;
                      const qint64 aa = qint64(a.width) * a.height;
                      const qint64 ab = qint64(b.width) * b.height;
                      if (aa != ab)
                          return aa > ab;
                      if (a.size != b.size)
                          return a.size > b.size;
                      if (a.mtime != b.mtime)
                          return a.mtime > b.mtime;
                      return a.path < b.path;
                  });
        qint64 total = 0;
        for (const DedupFile &f : group.files)
            total += f.size;
        group.wasted = qMax<qint64>(0, total - group.files.first().size);
    }
    std::sort(groups.begin(), groups.end(),
              [](const DedupGroup &a, const DedupGroup &b) {
                  if (a.wasted != b.wasted)
                      return a.wasted > b.wasted;
                  return a.files.size() > b.files.size();
              });

    m_tree->blockSignals(true);
    m_tree->clear();
    for (const DedupGroup &group : groups) {
        auto *top = new QTreeWidgetItem(m_tree);
        top->setText(3, tr("%1 copies · %2 reclaimable")
                            .arg(group.files.size())
                            .arg(formatBytes(group.wasted)));
        top->setText(6, dedupKindLabel(group.kind));
        top->setText(7, dedupScopeLabel(group.scope));
        const QPixmap topThumb = thumbFor(group.files.first());
        if (!topThumb.isNull())
            top->setIcon(2, QIcon(topThumb));
        top->setExpanded(groups.size() <= 40);
        for (const DedupFile &f : group.files) {
            auto *item = new QTreeWidgetItem(top);
            const bool keeper = (&f == &group.files.first());
            const bool canDelete = deletable(f);
            item->setData(0, PathRole, f.path);
            item->setData(0, ScopeRole, f.scope);
            item->setData(0, ShaRole, f.sha256);
            item->setData(0, IdRole, f.id);
            item->setData(0, RootRole, f.root);
            item->setData(0, KeeperRole, keeper);
            item->setData(0, DeletableRole, canDelete);
            if (keeper) {
                item->setText(1, QStringLiteral("★"));
                item->setForeground(1, QBrush(kKeepColor));
                item->setFlags(item->flags() & ~Qt::ItemIsUserCheckable);
            } else if (canDelete) {
                item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
                item->setCheckState(0, Qt::Checked);
            } else {
                item->setFlags(item->flags() & ~Qt::ItemIsUserCheckable);
                item->setToolTip(
                    0, tr("Archive deletion is disabled in Preferences → "
                          "Library"));
            }
            const QFileInfo fi(f.path);
            const QString rel = QDir(f.root).relativeFilePath(f.path);
            item->setText(3, QDir::toNativeSeparators(
                                rel == f.path ? fi.fileName() : rel));
            item->setToolTip(3, QDir::toNativeSeparators(f.path));
            item->setText(4, formatBytes(f.size));
            item->setText(5, f.width > 0 && f.height > 0
                                 ? QStringLiteral("%1×%2")
                                       .arg(f.width)
                                       .arg(f.height)
                                 : QStringLiteral("—"));
            const Meta meta = metaFor(f);
            item->setText(6, tr("%1 tags").arg(meta.tags));
            if (!meta.text.isEmpty())
                item->setToolTip(6, meta.text.left(2000));
            item->setText(7, dedupScopeLabel(f.scope));
            const QPixmap thumb = thumbFor(f);
            if (!thumb.isNull())
                item->setIcon(2, QIcon(thumb));
            item->setSizeHint(2, QSize(54, 54));
        }
    }
    m_tree->blockSignals(false);
    m_status->setText(
        tr("%1 duplicate group(s), %2 hashed file(s) shown.")
            .arg(groups.size())
            .arg(m_rows.size()));
    updateDeleteButton();
    showDetails();
}

DedupDialog::Meta DedupDialog::metaFor(const DedupFile &file) {
    const auto cached = m_meta.constFind(file.path);
    if (cached != m_meta.constEnd())
        return cached.value();
    Meta meta;
    if (file.scope == QStringLiteral("hydrus") && m_hydrus) {
        if (!file.sha256.isEmpty()) {
            const HydrusStore::Record r = m_hydrus->record(file.sha256);
            meta.tags = static_cast<int>(r.tags.size());
            meta.text = r.tags.join(u' ');
        }
    } else {
        const QString sidecar = localSidecarPath(file.path);
        if (!sidecar.isEmpty()) {
            QFile f(sidecar);
            if (f.size() > 0 && f.size() <= kMaxSidecar &&
                f.open(QIODevice::ReadOnly)) {
                QJsonParseError error;
                const QJsonDocument doc =
                    QJsonDocument::fromJson(f.readAll(), &error);
                if (error.error == QJsonParseError::NoError &&
                    doc.isObject()) {
                    Post post;
                    if (MetaParser::parse(doc.object(), post)) {
                        meta.tags = static_cast<int>(
                            post.tags.split(u' ', Qt::SkipEmptyParts).size());
                        meta.text = post.tags;
                    }
                }
            }
        }
    }
    meta.text = meta.text.left(100000);
    m_meta.insert(file.path, meta);
    return meta;
}

QPixmap DedupDialog::thumbFor(const DedupFile &file) {
    const auto cached = m_thumbs.constFind(file.path);
    if (cached != m_thumbs.constEnd())
        return cached.value();
    QPixmap pixmap;
    if (file.kind == QStringLiteral("video")) {
        pixmap = UiIcons::icon(QStringLiteral("play"))
                     .pixmap(QSize(64, 64));
    } else {
        QImageReader reader(file.path);
        reader.setAutoTransform(true);
        const QSize size = reader.size();
        if (size.isValid())
            reader.setScaledSize(size.scaled(72, 72, Qt::KeepAspectRatio));
        const QImage image = reader.read();
        if (!image.isNull())
            pixmap = QPixmap::fromImage(image.scaled(
                72, 72, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    }
    m_thumbs.insert(file.path, pixmap);
    return pixmap;
}

QString DedupDialog::scopeForItem(const QTreeWidgetItem *item) const {
    return item ? item->data(0, ScopeRole).toString() : QString();
}

QString DedupDialog::pathForItem(const QTreeWidgetItem *item) const {
    return item ? item->data(0, PathRole).toString() : QString();
}

bool DedupDialog::deletable(const DedupFile &file) const {
    if (file.scope == QStringLiteral("archive"))
        return m_sources.allowArchiveDelete;
    if (file.scope == QStringLiteral("hydrus"))
        return m_hydrus != nullptr; // needs the DB row removal too
    return file.scope == QStringLiteral("download");
}

bool DedupDialog::isUnder(const QString &path, const QString &dir) {
    if (path.isEmpty() || dir.isEmpty())
        return false;
    const QString d = QDir(dir).canonicalPath();
    if (d.isEmpty())
        return false;
    const QString p = QFileInfo(path).canonicalFilePath();
    const QString use = p.isEmpty() ? QFileInfo(path).absoluteFilePath() : p;
    return use == d || use.startsWith(d + u'/');
}

bool DedupDialog::pathAllowed(const DedupFile &file) const {
    if (file.scope == QStringLiteral("download"))
        return isUnder(file.path, m_sources.downloadDir);
    if (file.scope == QStringLiteral("hydrus"))
        return isUnder(file.path, m_sources.hydrusDir);
    if (file.scope == QStringLiteral("archive")) {
        if (!m_sources.allowArchiveDelete)
            return false;
        for (const QString &root : allArchiveRoots()) {
            if (isUnder(file.path, root))
                return true;
        }
        return false;
    }
    return false;
}

QString DedupDialog::formatBytes(qint64 bytes) {
    const double value = static_cast<double>(qMax<qint64>(0, bytes));
    if (value >= 1024.0 * 1024.0 * 1024.0)
        return QStringLiteral("%1 GB").arg(value / (1024.0 * 1024.0 * 1024.0),
                                          0, 'f', 1);
    if (value >= 1024.0 * 1024.0)
        return QStringLiteral("%1 MB").arg(value / (1024.0 * 1024.0), 0, 'f',
                                           1);
    if (value >= 1024.0)
        return QStringLiteral("%1 KB").arg(value / 1024.0, 0, 'f', 1);
    return QStringLiteral("%1 B").arg(qint64(value));
}

void DedupDialog::keepSelected() {
    QTreeWidgetItem *item = m_tree->currentItem();
    if (!item || !item->parent())
        return;
    QTreeWidgetItem *group = item->parent();
    m_tree->blockSignals(true);
    for (int i = 0; i < group->childCount(); ++i) {
        QTreeWidgetItem *child = group->child(i);
        const bool keep = child == item;
        child->setData(0, KeeperRole, keep);
        if (keep) {
            child->setText(1, QStringLiteral("★"));
            child->setForeground(1, QBrush(kKeepColor));
            child->setFlags(child->flags() & ~Qt::ItemIsUserCheckable);
            child->setCheckState(0, Qt::Unchecked);
        } else {
            child->setText(1, QString());
            child->setForeground(1, QBrush());
            if (child->data(0, DeletableRole).toBool()) {
                child->setFlags(child->flags() | Qt::ItemIsUserCheckable);
                child->setCheckState(0, Qt::Checked);
            } else {
                child->setFlags(child->flags() & ~Qt::ItemIsUserCheckable);
                child->setCheckState(0, Qt::Unchecked);
            }
        }
    }
    m_tree->blockSignals(false);
    updateDeleteButton();
    showDetails();
}

void DedupDialog::updateDeleteButton() {
    int selected = 0;
    if (m_tree) {
        for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
            const QTreeWidgetItem *group = m_tree->topLevelItem(i);
            for (int j = 0; j < group->childCount(); ++j)
                if (group->child(j)->checkState(0) == Qt::Checked)
                    ++selected;
        }
    }
    if (m_deleteBtn) {
        m_deleteBtn->setText(tr("Delete selected (%1)…").arg(selected));
        m_deleteBtn->setEnabled(!m_scanning && selected > 0);
    }
    if (m_keepBtn) {
        const QTreeWidgetItem *cur = m_tree ? m_tree->currentItem() : nullptr;
        m_keepBtn->setEnabled(!m_scanning && cur && cur->parent());
    }
}

void DedupDialog::showDetails() {
    if (!m_details || !m_tree)
        return;
    QTreeWidgetItem *item = m_tree->currentItem();
    if (!item) {
        m_details->clear();
        return;
    }
    const QString path = pathForItem(item);
    if (path.isEmpty()) { // group header
        m_details->setPlainText(
            tr("%1\n%2 · %3")
                .arg(item->text(3))
                .arg(item->text(6))
                .arg(item->text(7)));
        return;
    }
    const QString scope = scopeForItem(item);
    const auto it = m_rows.constFind(rowKey(scope, path));
    if (it == m_rows.constEnd()) {
        m_details->clear();
        return;
    }
    const DedupFile &file = it.value();
    const Meta meta = metaFor(file);
    QString text = tr("%1\n%2 · %3 · %4 · %5 tags\nSHA-256: %6")
                       .arg(QDir::toNativeSeparators(file.path))
                       .arg(dedupScopeLabel(file.scope))
                       .arg(dedupKindLabel(file.kind))
                       .arg(formatBytes(file.size))
                       .arg(meta.tags)
                       .arg(file.sha256);
    if (file.hasPhash)
        text += tr("\npHash: %1").arg(file.phash, 16, 16, QChar(u'0'));
    if (!meta.text.isEmpty())
        text += QStringLiteral("\n\n") + meta.text;
    m_details->setPlainText(text);
}

void DedupDialog::deleteSelected() {
    QVector<DedupFile> pending;
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem *group = m_tree->topLevelItem(i);
        for (int j = 0; j < group->childCount(); ++j) {
            QTreeWidgetItem *child = group->child(j);
            if (child->checkState(0) != Qt::Checked)
                continue;
            const QString scope = scopeForItem(child);
            const QString path = pathForItem(child);
            const auto it = m_rows.constFind(rowKey(scope, path));
            if (it == m_rows.constEnd())
                continue;
            if (!pathAllowed(it.value())) {
                QMessageBox::warning(
                    this, tr("Duplicate media"),
                    tr("Refusing to delete a path outside the selected "
                       "source:\n%1")
                        .arg(QDir::toNativeSeparators(path)));
                continue;
            }
            pending.append(it.value());
        }
    }
    if (pending.isEmpty())
        return;

    qint64 bytes = 0;
    int archiveCount = 0;
    int hydrusCount = 0;
    for (const DedupFile &f : pending) {
        bytes += f.size;
        if (f.scope == QStringLiteral("archive"))
            ++archiveCount;
        else if (f.scope == QStringLiteral("hydrus"))
            ++hydrusCount;
    }
    QString text =
        tr("Move %1 file(s) (%2) to the trash?\n\n"
           "Their JSON sidecars are removed too%3%4.")
            .arg(pending.size())
            .arg(formatBytes(bytes))
            .arg(archiveCount > 0
                     ? tr("; %1 come from local archives").arg(archiveCount)
                     : QString())
            .arg(hydrusCount > 0
                     ? tr("; %1 Hydrus row(s) are removed from hydrus.db")
                           .arg(hydrusCount)
                     : QString());
    QMessageBox box(QMessageBox::Question, tr("Delete duplicates"), text,
                    QMessageBox::NoButton, this);
    QPushButton *move = box.addButton(tr("Move to Trash"),
                                      QMessageBox::AcceptRole);
    box.addButton(QMessageBox::Cancel);
    box.exec();
    if (box.clickedButton() != move)
        return;

    QStringList deletedPaths;
    QStringList archiveRoots;
    QStringList failures;
    QStringList seenRoots;
    QStringList hydrusRowFailures;

    auto finishDeletion = [&](const DedupFile &file) {
        if (file.scope != QStringLiteral("hydrus")) {
            for (const QString &sidecar :
                 DownloadManager::sidecarPathsFor(file.path))
                QFile::remove(sidecar);
        } else if (m_hydrus && !file.sha256.isEmpty()) {
            if (!m_hydrus->remove(file.sha256))
                hydrusRowFailures.append(file.sha256);
        }
        m_db.removePath(file.scope, file.path);
        m_rows.remove(rowKey(file.scope, file.path));
        m_meta.remove(file.path);
        m_thumbs.remove(file.path);
        deletedPaths.append(file.path);
        if (file.scope == QStringLiteral("archive") &&
            !seenRoots.contains(file.root)) {
            seenRoots.append(file.root);
            archiveRoots.append(file.root);
        }
    };

    for (const DedupFile &file : pending) {
        bool ok = QFile::moveToTrash(file.path);
        if (!ok && !QFileInfo::exists(file.path))
            ok = true; // already gone; drop the cached row
        if (!ok) {
            failures.append(file.path);
            continue;
        }
        finishDeletion(file);
    }

    if (!failures.isEmpty()) {
        QMessageBox retry(
            QMessageBox::Question, tr("Delete duplicates"),
            tr("%1 file(s) could not be moved to the trash. Permanently "
               "delete them instead?\n\n%2")
                .arg(failures.size())
                .arg(QDir::toNativeSeparators(failures.first())),
            QMessageBox::NoButton, this);
        QPushButton *permanent = retry.addButton(
            tr("Delete permanently"), QMessageBox::DestructiveRole);
        retry.addButton(QMessageBox::Cancel);
        retry.exec();
        if (retry.clickedButton() == permanent) {
            for (const QString &path : failures) {
                DedupFile file;
                bool found = false;
                for (const QString &scope : {QStringLiteral("archive"),
                                             QStringLiteral("download"),
                                             QStringLiteral("hydrus")}) {
                    const auto hit = m_rows.constFind(rowKey(scope, path));
                    if (hit != m_rows.constEnd()) {
                        file = hit.value();
                        found = true;
                        break;
                    }
                }
                if (!found || !pathAllowed(file))
                    continue;
                if (QFile::remove(path) || !QFileInfo::exists(path))
                    finishDeletion(file);
            }
        }
    }

    if (!deletedPaths.isEmpty())
        emit filesDeleted(deletedPaths, archiveRoots);
    reloadGroups();
    if (!deletedPaths.isEmpty())
        m_status->setText(
            tr("Deleted %1 file(s) (%2).")
                .arg(deletedPaths.size())
                .arg(formatBytes(bytes)));
    if (!hydrusRowFailures.isEmpty())
        QMessageBox::warning(
            this, tr("Delete duplicates"),
            tr("Media was deleted, but %1 Hydrus database row(s) could not be "
               "removed.")
                .arg(hydrusRowFailures.size()));
    if (!failures.isEmpty() && deletedPaths.isEmpty())
        QMessageBox::warning(this, tr("Delete duplicates"),
                             tr("No files could be deleted."));
}

void DedupDialog::clearDatabase() {
    if (QMessageBox::question(
            this, tr("Forget cached hashes"),
            tr("Discard every cached hash? The next scan will rebuild them "
               "from the files. Nothing on disk is deleted."),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No) != QMessageBox::Yes)
        return;
    m_db.clear();
    m_rows.clear();
    m_meta.clear();
    m_thumbs.clear();
    reloadGroups();
    m_status->setText(tr("Cached hashes cleared."));
}

void DedupDialog::closeEvent(QCloseEvent *event) {
    if (m_scanning) {
        m_closeWhenDone = true;
        if (m_task)
            m_task->requestCancel();
        m_status->setText(tr("Stopping the scan…"));
        event->ignore();
        return;
    }
    QDialog::closeEvent(event);
}
