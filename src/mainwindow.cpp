#include "mainwindow.h"
#include "cache.h"
#include "prefsdlg.h"
#include "scanner.h"
#include "tagtypes.h"
#include "theme.h"
#include "thumbloader.h"
#include "viewerpage.h"
#include "welcomepage.h"

#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QCompleter>
#include <QDesktopServices>
#include <QDir>
#include <QDrag>
#include <QFileDialog>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QPushButton>
#include <QProgressBar>
#include <QScrollBar>
#include <QSet>
#include <QShortcut>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStringListModel>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent>
#include <climits>

// ── rating colors ────────────────────────────────────────────────

namespace {
QColor ratingColor(const QString &r) {
    if (r == QStringLiteral("sensitive"))
        return QColor(0xcc, 0xaa, 0x6f);
    if (r == QStringLiteral("questionable"))
        return QColor(0xcc, 0x80, 0x60);
    if (r == QStringLiteral("explicit"))
        return QColor(0xcc, 0x55, 0x55);
    return QColor(0x6f, 0xcc, 0x6f);
}
constexpr int kBoxW = 184;
} // namespace

// ── GalleryDelegate ──────────────────────────────────────────────

GalleryDelegate::GalleryDelegate(QObject *parent) : QStyledItemDelegate(parent) {}

void GalleryDelegate::setAspect(int num, int den) {
    m_num = qMax(1, num);
    m_den = qMax(1, den);
}

QSize GalleryDelegate::sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const {
    const int boxH = qMax(60, (kBoxW * m_den) / m_num);
    return {kBoxW + 12, boxH + 62};
}

void GalleryDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option,
                            const QModelIndex &index) const {
    painter->save();
    const int boxH = qMax(60, (kBoxW * m_den) / m_num);
    QRect r = option.rect.adjusted(4, 4, -4, -4);

    const bool sel = option.state & QStyle::State_Selected;
    painter->setRenderHint(QPainter::Antialiasing);
    painter->setPen(sel ? QColor(0x7c, 0x9e, 0xf8) : QColor(0x2a, 0x2f, 0x45));
    painter->setBrush(sel ? QColor(0x1e, 0x26, 0x3a) : QColor(0x16, 0x19, 0x20));
    painter->drawRoundedRect(r, 6, 6);

    // Thumbnail (loader already crops to aspect; expand-fill as safety).
    const QPixmap pm = qvariant_cast<QPixmap>(index.data(Qt::DecorationRole));
    const QRect imgR(r.left() + 5, r.top() + 5, r.width() - 10, boxH);
    painter->setClipRect(r);
    if (!pm.isNull()) {
        const QPixmap scaled =
            pm.scaled(imgR.size(), Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
        painter->drawPixmap(imgR.center().x() - scaled.width() / 2,
                            imgR.center().y() - scaled.height() / 2, scaled);
    } else {
        painter->fillRect(imgR, QColor(0x1e, 0x21, 0x30));
    }
    painter->setClipping(false);

    // Video / GIF badge (top-right over image).
    const QString kind = index.data(Qt::UserRole + 2).toString();
    if (!kind.isEmpty()) {
        const QRect kb(imgR.right() - 34, imgR.top() + 4, 30, 18);
        painter->setPen(Qt::NoPen);
        painter->setBrush(QColor(0, 0, 0, 180));
        painter->drawRoundedRect(kb, 4, 4);
        painter->setPen(QColor(0x7c, 0x9e, 0xf8));
        painter->drawText(kb, Qt::AlignCenter, kind);
    }

    // Unified info card: [G] +12 #11984812
    const QRect cardR(r.left() + 5, imgR.bottom() + 4, r.width() - 10, 24);
    painter->setPen(QColor(0x2a, 0x2f, 0x45));
    painter->setBrush(QColor(0x10, 0x12, 0x18));
    painter->drawRoundedRect(cardR, 4, 4);
    const QString rating = index.data(Qt::UserRole + 1).toString();
    const QRect chip(cardR.left() + 3, cardR.top() + 3, 20, 18);
    painter->setPen(Qt::NoPen);
    painter->setBrush(ratingColor(rating));
    painter->drawRoundedRect(chip, 3, 3);
    painter->setPen(Qt::black);
    QFont bf = painter->font();
    bf.setBold(true);
    bf.setPointSize(bf.pointSize() - 1);
    painter->setFont(bf);
    painter->drawText(chip, Qt::AlignCenter, rating.left(1).toUpper());
    painter->setFont(option.font);
    const QString score = index.data(Qt::UserRole + 3).toString();
    painter->setPen(score.startsWith(u'+')   ? QColor(0x6f, 0xcc, 0x6f)
                     : score.startsWith(u'-') ? QColor(0xe0, 0x55, 0x55)
                                              : QColor(0xd4, 0xd8, 0xf0));
    const QRect scoreR(chip.right() + 5, cardR.top(), 70, cardR.height());
    painter->drawText(scoreR, Qt::AlignVCenter | Qt::AlignLeft, score);
    painter->setPen(QColor(0x78, 0x80, 0xa0));
    const QRect idR(scoreR.right() + 2, cardR.top(), cardR.right() - scoreR.right() - 6,
                    cardR.height());
    painter->drawText(idR, Qt::AlignVCenter | Qt::AlignRight,
                      index.data(Qt::UserRole + 6).toString());

    // Filename line.
    painter->setPen(sel ? Qt::white : QColor(0x4a, 0x50, 0x68));
    const QRect labelR(r.left() + 6, cardR.bottom() + 3, r.width() - 12, 16);
    const QString name = index.data(Qt::DisplayRole).toString();
    painter->drawText(labelR, Qt::AlignHCenter | Qt::AlignTop,
                      painter->fontMetrics().elidedText(name, Qt::ElideMiddle,
                                                        labelR.width()));
    painter->restore();
}

// ── GridView ─────────────────────────────────────────────────────

GridView::GridView(QWidget *parent) : QListView(parent) {
    setDragEnabled(true);
    setDragDropMode(QAbstractItemView::DragOnly);
    setSelectionMode(QAbstractItemView::ExtendedSelection);
}

void GridView::startDrag(Qt::DropActions) {
    const QModelIndexList sel = selectedIndexes();
    QList<QUrl> urls;
    for (const QModelIndex &i : sel) {
        const QString f = model()->data(i, Qt::UserRole + 5).toString();
        if (!f.isEmpty())
            urls << QUrl::fromLocalFile(f);
    }
    if (urls.isEmpty())
        return;
    auto *mime = new QMimeData();
    mime->setUrls(urls);
    auto *drag = new QDrag(this);
    drag->setMimeData(mime);
    const QPixmap pm =
        qvariant_cast<QPixmap>(model()->data(sel.first(), Qt::DecorationRole));
    if (!pm.isNull())
        drag->setPixmap(pm.scaled(128, 128, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    drag->exec(Qt::CopyAction);
}

// ── MainWindow ───────────────────────────────────────────────────

class MainWindow::ScanWatcher : public QFutureWatcher<QVector<Post>> {};

MainWindow::MainWindow(const QString &root, bool rootExplicit, QWidget *parent)
    : QMainWindow(parent), m_thumbs(new ThumbLoader(this)),
      m_videoThumbs(new VideoThumbGrabber(this)),
      m_fetcher(new TagTypeFetcher(this)) {
    m_settings = loadSettings();
    setActiveExplicitTags(
        QSet<QString>(m_settings.explicitTags.begin(), m_settings.explicitTags.end()));
    // Remembered folder wins when no explicit root was given on the CLI.
    m_root = root;
    if (!rootExplicit && !m_settings.lastRoot.isEmpty() &&
        QDir(m_settings.lastRoot).exists())
        m_root = m_settings.lastRoot;
    setWindowTitle(QStringLiteral("booru-explorer"));
    resize(1280, 800);
    if (!m_settings.geometry.isEmpty())
        restoreGeometry(m_settings.geometry);

    applyThemeFromSettings();
    const QString thumbDir = appCacheDir() + u'/' + QStringLiteral("thumbs");
    m_thumbs->setCacheDir(m_settings.thumbCache ? thumbDir : QString());
    m_videoThumbs->setCacheDir(m_settings.thumbCache ? thumbDir : QString());

    auto *fileMenu = menuBar()->addMenu(QStringLiteral("&File"));
    fileMenu->addAction(QStringLiteral("Open &Folder…"), this, &MainWindow::openFolder);
    fileMenu->addAction(QStringLiteral("&Close Folder"), this, &MainWindow::showWelcome);
    auto *reloadAct =
        fileMenu->addAction(QStringLiteral("&Reload"), this, &MainWindow::reload);
    reloadAct->setShortcut(QKeySequence::Refresh); // F5
    fileMenu->addAction(QStringLiteral("Load Custom &Stylesheet…"), this,
                        &MainWindow::loadStylesheet);
    fileMenu->addSeparator();
    fileMenu->addAction(QStringLiteral("&Quit"), qApp, &QApplication::quit);

    auto *editMenu = menuBar()->addMenu(QStringLiteral("&Edit"));
    auto *prefsAct = editMenu->addAction(QStringLiteral("&Preferences…"), this,
                                         &MainWindow::openPreferences);
    prefsAct->setShortcut(QKeySequence::Preferences);
    // Keyboard: arrows navigate the grid natively, Enter opens.
    // "/" focuses search, Esc clears filters (or leaves the viewer).
    auto *focusSearch = new QShortcut(QKeySequence(QStringLiteral("/")), this);
    focusSearch->setContext(Qt::ApplicationShortcut);
    connect(focusSearch, &QShortcut::activated, this, [this] {
        if (m_pages->currentIndex() == 0) {
            m_searchEdit->setFocus();
            m_searchEdit->selectAll();
        }
    });
    auto *esc = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    esc->setContext(Qt::ApplicationShortcut);
    connect(esc, &QShortcut::activated, this, [this] {
        if (m_pages->currentIndex() == 1) {
            backToGrid();
        } else {
            m_searchEdit->clear();
            m_searchQuery.clear();
            m_activeTags.clear();
            rebuildSidebar();
            applyFilter();
        }
    });

    // ── Omnibar: search + rating + source + sort + ratio + reload ──
    auto *omni = new QWidget(this);
    auto *omniLay = new QHBoxLayout(omni);
    omniLay->setContentsMargins(8, 6, 8, 6);
    omniLay->setSpacing(6);
    m_searchEdit = new QLineEdit(this);
    m_searchEdit->setPlaceholderText(QStringLiteral("Search tags… prefix - to exclude"));
    m_searchEdit->setClearButtonEnabled(true);
    m_searchEdit->setMinimumWidth(180);
    m_completeModel = new QStringListModel(this);
    m_completer = new QCompleter(m_completeModel, this);
    m_completer->setCaseSensitivity(Qt::CaseInsensitive);
    m_completer->setCompletionMode(QCompleter::PopupCompletion);
    m_searchEdit->setCompleter(m_completer);
    m_completeTimer = new QTimer(this);
    m_completeTimer->setSingleShot(true);
    m_completeTimer->setInterval(150);
    connect(m_searchEdit, &QLineEdit::textEdited, m_completeTimer,
            static_cast<void (QTimer::*)()>(&QTimer::start));
    connect(m_completeTimer, &QTimer::timeout, this,
            [this] { onSearchEdited(m_searchEdit->text()); });
    connect(m_searchEdit, &QLineEdit::returnPressed, this, &MainWindow::onSearchReturn);
    connect(m_completer, QOverload<const QString &>::of(&QCompleter::activated), this,
            &MainWindow::onCompleterActivated);

    m_ratingBox = new QComboBox(this);
    m_categoryBox = new QComboBox(this);
    m_sortBox = new QComboBox(this);
    m_sortBox->addItem(QStringLiteral("Newest"), QStringLiteral("id-desc"));
    m_sortBox->addItem(QStringLiteral("Oldest"), QStringLiteral("id-asc"));
    m_sortBox->addItem(QStringLiteral("Top Rated"), QStringLiteral("score-desc"));
    m_sortBox->addItem(QStringLiteral("Low Rated"), QStringLiteral("score-asc"));
    m_sortBox->addItem(QStringLiteral("Random"), QStringLiteral("random"));
    m_aspectBox = new QComboBox(this);
    m_aspectBox->addItem(QStringLiteral("1:1"), QVariantList{1, 1});
    m_aspectBox->addItem(QStringLiteral("4:3"), QVariantList{4, 3});
    m_aspectBox->addItem(QStringLiteral("3:2"), QVariantList{3, 2});
    m_aspectBox->addItem(QStringLiteral("16:9"), QVariantList{16, 9});
    m_aspectBox->addItem(QStringLiteral("3:4"), QVariantList{3, 4});
    m_aspectBox->addItem(QStringLiteral("Custom…"), QVariantList{});
    connect(m_sortBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this] {
                m_sort = m_sortBox->currentData().toString();
                applyFilter();
            });
    connect(m_ratingBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &MainWindow::onRatingChanged);
    connect(m_categoryBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &MainWindow::onCategoryChanged);
    connect(m_aspectBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &MainWindow::onAspectChanged);

    auto *clearBtn = new QPushButton(QStringLiteral("Clear"), this);
    auto *reloadBtn = new QPushButton(QStringLiteral("↺"), this);
    reloadBtn->setToolTip(QStringLiteral("Re-scan folder"));
    connect(clearBtn, &QPushButton::clicked, this, [this] {
        m_searchEdit->clear();
        m_searchQuery.clear();
        m_activeTags.clear();
        rebuildSidebar();
        applyFilter();
    });
    connect(reloadBtn, &QPushButton::clicked, this, &MainWindow::reload);
    m_countLabel = new QLabel(this);
    m_countLabel->setStyleSheet(QStringLiteral("color:#4a5068;"));
    m_resultLabel = new QLabel(this);
    m_resultLabel->setStyleSheet(QStringLiteral("color:#4a5068;"));

    omniLay->addWidget(m_searchEdit, 1);
    omniLay->addWidget(m_ratingBox);
    omniLay->addWidget(m_categoryBox);
    omniLay->addWidget(m_sortBox);
    omniLay->addWidget(m_aspectBox);
    omniLay->addWidget(clearBtn);
    omniLay->addWidget(reloadBtn);
    omniLay->addWidget(m_countLabel);
    omniLay->addWidget(m_resultLabel);

    // ── Sidebar: the one tag menu, booru order, mouse-resizable sections ──
    auto *side = new QWidget(this);
    auto *sideLay = new QVBoxLayout(side);
    sideLay->setContentsMargins(4, 4, 4, 4);
    m_sideSplit = new QSplitter(Qt::Vertical, this);
    m_copyList = new QListWidget(this);
    m_charList = new QListWidget(this);
    m_artistList = new QListWidget(this);
    m_tagList = new QListWidget(this);
    m_metaList = new QListWidget(this);
    // Real booru order: Copyright, Characters, Artists, general, meta.
    const QList<QPair<QString, QListWidget *>> sections = {
        {QStringLiteral("COPYRIGHT"), m_copyList},
        {QStringLiteral("CHARACTERS"), m_charList},
        {QStringLiteral("ARTISTS"), m_artistList},
        {QStringLiteral("TAGS"), m_tagList},
        {QStringLiteral("META"), m_metaList},
    };
    for (const auto &[title, list] : sections) {
        auto *sec = new QWidget(this);
        auto *secLay = new QVBoxLayout(sec);
        secLay->setContentsMargins(0, 0, 0, 0);
        secLay->setSpacing(2);
        auto *h = new QLabel(title, this);
        h->setStyleSheet(
            QStringLiteral("color:#4a5068;font-size:10px;font-weight:bold;"));
        secLay->addWidget(h);
        secLay->addWidget(list, 1);
        list->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        sec->setMinimumHeight(52); // header stays reachable when collapsed
        m_sideSplit->addWidget(sec);
        m_sideSplit->setStretchFactor(m_sideSplit->count() - 1, 1);
        connect(list, &QListWidget::itemClicked, this, &MainWindow::onTagClicked);
    }
    if (!m_settings.sideSplitter.isEmpty())
        m_sideSplit->restoreState(m_settings.sideSplitter);
    connect(m_sideSplit, &QSplitter::splitterMoved, this, [this] {
        m_settings.sideSplitter = m_sideSplit->saveState();
        saveSettings(m_settings);
    });
    sideLay->addWidget(m_sideSplit, 1);
    side->setFixedWidth(230);

    // ── Pages: grid + same-window viewer ──
    m_pages = new QStackedWidget(this);
    m_grid = new GridView(this);
    m_grid->setViewMode(QListView::IconMode);
    m_grid->setResizeMode(QListView::Adjust);
    m_grid->setMovement(QListView::Static);
    m_grid->setSpacing(4);
    m_grid->setUniformItemSizes(true);
    m_delegate = new GalleryDelegate(this);
    m_grid->setItemDelegate(m_delegate);
    m_model = new QStandardItemModel(this);
    m_grid->setModel(m_model);
    connect(m_grid, &QListView::doubleClicked, this, &MainWindow::onGridActivated);
    connect(m_grid, &QListView::activated, this, &MainWindow::onGridActivated);
    connect(m_thumbs, &ThumbLoader::thumbnailReady, this, &MainWindow::onThumbnailReady);
    connect(m_videoThumbs, &VideoThumbGrabber::thumbnailReady, this,
            &MainWindow::onThumbnailReady);
    connect(m_fetcher, &TagTypeFetcher::progress, this,
            &MainWindow::onClassifyProgress);
    connect(m_fetcher, &TagTypeFetcher::finished, this,
            &MainWindow::onClassifyFinished);
    connect(m_fetcher, &TagTypeFetcher::gelbooruAuthFailed, this, [this] {
        statusBar()->showMessage(
            QStringLiteral("Gelbooru rejected the API credentials — using "
                           "public sources (check User ID + key in Preferences)"),
            10000);
    });
    connect(m_fetcher, &TagTypeFetcher::backoff, this, [this] {
        statusBar()->showMessage(
            QStringLiteral("Tag API rate-limited — pausing briefly…"), 6000);
    });
    m_fetcher->setGelbooruCredentials(m_settings.gelbooruUser, m_settings.gelbooruKey);
    m_viewer = new ViewerPage(this);
    connect(m_viewer, &ViewerPage::back, this, &MainWindow::backToGrid);
    m_viewer->setPlaybackDefaults(m_settings.videoMuted, m_settings.videoLoop,
                                  m_settings.volume);
    m_viewer->setMaxSide(m_settings.viewerMaxSide);
    connect(m_viewer, &ViewerPage::tagActivated, this, [this](const QString &tag) {
        if (!m_activeTags.contains(tag))
            m_activeTags.append(tag);
        rebuildSidebar();
        applyFilter();
        backToGrid();
    });
    m_pages->addWidget(m_grid);
    m_pages->addWidget(m_viewer);

    m_thumbTimer = new QTimer(this);
    m_thumbTimer->setSingleShot(true);
    m_thumbTimer->setInterval(120);
    connect(m_thumbTimer, &QTimer::timeout, this, &MainWindow::requestVisibleThumbs);
    connect(m_grid->verticalScrollBar(), &QScrollBar::valueChanged, m_thumbTimer,
            static_cast<void (QTimer::*)()>(&QTimer::start));

    auto *split = new QSplitter(this);
    split->addWidget(side);
    auto *center = new QWidget(this);
    auto *centerLay = new QVBoxLayout(center);
    centerLay->setContentsMargins(0, 0, 0, 0);
    centerLay->addWidget(omni);
    // Active-filter pills (replaces the old Active tab).
    m_pillBar = new QWidget(this);
    auto *pillLay = new QHBoxLayout(m_pillBar);
    pillLay->setContentsMargins(8, 0, 8, 4);
    m_pills = new FlowLayout(nullptr, 0, 4, 4);
    pillLay->addLayout(m_pills);
    m_pillBar->setVisible(false);
    centerLay->addWidget(m_pillBar);
    centerLay->addWidget(m_pages, 1);
    split->addWidget(center);
    split->setStretchFactor(1, 1);

    // Stage: welcome page vs. browser.
    m_welcome = new WelcomePage(this);
    connect(m_welcome, &WelcomePage::openDialogRequested, this, &MainWindow::openFolder);
    connect(m_welcome, &WelcomePage::openPathRequested, this, &MainWindow::openPath);
    connect(m_welcome, &WelcomePage::restoreToggled, this, [this](bool on) {
        m_settings.restoreSession = on;
        saveSettings(m_settings);
    });
    m_stage = new QStackedWidget(this);
    m_stage->addWidget(m_welcome);
    m_stage->addWidget(split);
    setCentralWidget(m_stage);
    m_scanBar = new QProgressBar(this);
    m_scanBar->setFixedWidth(180);
    m_scanBar->setTextVisible(true);
    m_scanBar->setVisible(false);
    statusBar()->addPermanentWidget(m_scanBar);
    statusBar()->showMessage(QStringLiteral("Ready"));

    m_placeholder = QPixmap(kBoxW, kBoxW);
    m_placeholder.fill(QColor(0x1e, 0x21, 0x30));

    setAspect(m_settings.aspectNum, m_settings.aspectDen);

    // Startup routing: explicit CLI root always opens; otherwise the saved
    // session wins only when opted in, else the welcome page shows history.
    const bool canResume = !m_settings.lastRoot.isEmpty() &&
                           QDir(m_settings.lastRoot).exists();
    if (rootExplicit) {
        startScan(m_root);
    } else if (m_settings.restoreSession && canResume) {
        m_root = m_settings.lastRoot;
        startScan(m_root);
    } else {
        showWelcome();
    }
}

void MainWindow::openPath(const QString &path) {
    if (path.isEmpty() || !QDir(path).exists())
        return;
    startScan(path);
}

void MainWindow::showWelcome() {
    // Prune dead entries so the list never rots.
    QStringList kept;
    for (const QString &p : m_settings.history) {
        if (QDir(p).exists() && !kept.contains(p))
            kept.append(p);
    }
    if (kept != m_settings.history) {
        m_settings.history = kept.mid(0, 10);
        saveSettings(m_settings);
    }
    m_welcome->setHistory(m_settings.history, m_settings.lastRoot);
    m_welcome->setRestore(m_settings.restoreSession);
    m_stage->setCurrentIndex(0);
    setWindowTitle(QStringLiteral("booru-explorer"));
}

void MainWindow::openFolder() {
    const QString dir = QFileDialog::getExistingDirectory(
        this, QStringLiteral("Open booru folder"), m_root);
    if (dir.isEmpty())
        return;
    m_settings.lastRoot = dir;
    saveSettings(m_settings);
    startScan(dir);
}

void MainWindow::reload() {
    startScan(m_root);
}

void MainWindow::loadStylesheet() {
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Load custom Qt stylesheet"), QDir::homePath(),
        QStringLiteral("Stylesheets (*.qss);;All files (*)"));
    if (path.isEmpty())
        return;
    if (!Theme::applyStylesheetFile(*qApp, path))
        QMessageBox::warning(this, QStringLiteral("Stylesheet"),
                             QStringLiteral("Could not read %1").arg(path));
}

void MainWindow::startScan(const QString &root) {
    m_fetcher->cancel();
    m_root = root;
    m_stage->setCurrentIndex(1);
    setWindowTitle(QStringLiteral("booru-explorer — %1").arg(root));
    statusBar()->showMessage(QStringLiteral("Scanning %1 …").arg(root));
    auto *scanner = new Scanner;
    auto *watcher = new ScanWatcher;
    connect(scanner, &Scanner::progress, this, &MainWindow::onScanProgress);
    connect(scanner, &Scanner::cacheStats, this, &MainWindow::onCacheStats);
    connect(watcher, &QFutureWatcher<QVector<Post>>::finished, this,
            [this, watcher, scanner] {
                m_index.setPosts(watcher->result());
                watcher->deleteLater();
                scanner->deleteLater();
                onScanFinished();
            });
    watcher->setFuture(QtConcurrent::run(
        &Scanner::scan, root, scanner, m_settings.indexCache,
        static_cast<QSet<QString> *>(nullptr)));
}

void MainWindow::onScanProgress(int n, int total) {
    m_scanBar->setVisible(true);
    m_scanBar->setRange(0, qMax(1, total));
    m_scanBar->setValue(n);
    statusBar()->showMessage(QStringLiteral("Scanning… %1 / %2 files").arg(n).arg(total));
}

void MainWindow::onCacheStats(int hits, int misses) {
    statusBar()->showMessage(
        QStringLiteral("Index cache: %1 reused, %2 parsed").arg(hits).arg(misses), 8000);
}

void MainWindow::onScanFinished() {
    const Stats s = m_index.stats();
    m_countLabel->setText(QStringLiteral("%1 posts").arg(s.total));
    m_scanBar->setVisible(false);
    statusBar()->showMessage(QStringLiteral("Found %1 posts").arg(s.total), 5000);
    // Archive history (most-recent-first, capped) + last session.
    m_settings.lastRoot = m_root;
    m_settings.history.removeAll(m_root);
    m_settings.history.prepend(m_root);
    m_settings.history = m_settings.history.mid(0, 10);
    saveSettings(m_settings);
    rebuildCombos();
    rebuildSidebar();
    applyFilter();
    maybeClassify();
}

void MainWindow::maybeClassify() {
    if (!m_settings.classifyTags)
        return;
    const QSet<QString> unknown = m_index.collectUntyped();
    if (unknown.isEmpty())
        return;
    const QString db = ScanCache::dbPathFor(QDir(m_root).absolutePath());
    TagTypeCache cache;
    QHash<QString, QString> known;
    QStringList missing;
    if (cache.open(db)) {
        for (const QString &t : unknown) {
            int ty = TagTypes::kTransientFailure;
            if (cache.get(t, ty)) {
                if (ty != TagTypes::kUnknownTag)
                    known.insert(t, TagTypes::bucketForType(ty));
                // negatives stay general without refetching
            } else {
                missing.append(t);
            }
        }
        cache.close();
    } else {
        missing = QStringList(unknown.begin(), unknown.end());
    }
    if (!known.isEmpty()) {
        const int moved = m_index.applyTagTypes(known);
        if (moved > 0) {
            rebuildCombos();
            rebuildSidebar();
            applyFilter();
            statusBar()->showMessage(
                QStringLiteral("Categorized %1 tags from cache").arg(moved), 8000);
        }
    }
    if (!missing.isEmpty()) {
        missing.sort();
        statusBar()->showMessage(
            QStringLiteral("Classifying %1 untyped tags…").arg(missing.size()));
        m_fetcher->fetch(missing, db);
    }
}

void MainWindow::onClassifyProgress(int done, int total) {
    m_scanBar->setVisible(true);
    m_scanBar->setRange(0, qMax(1, total));
    m_scanBar->setValue(done);
    statusBar()->showMessage(
        QStringLiteral("Classifying tags… %1 / %2").arg(done).arg(total));
}

void MainWindow::onClassifyFinished(const QHash<QString, QString> &buckets) {
    m_scanBar->setVisible(false);
    const int failed = m_fetcher->failures();
    if (buckets.isEmpty()) {
        statusBar()->showMessage(
            failed > 0
                ? QStringLiteral("Tag classification: %1 failed (network), "
                                 "will retry next scan")
                      .arg(failed)
                : QStringLiteral("Tag classification done"),
            8000);
        return;
    }
    const int moved = m_index.applyTagTypes(buckets);
    rebuildCombos();
    rebuildSidebar();
    applyFilter();
    QString msg = QStringLiteral("Categorized %1 tags (%2 resolved)")
                      .arg(moved)
                      .arg(buckets.size());
    if (failed > 0)
        msg += QStringLiteral(" · %1 failed, will retry").arg(failed);
    statusBar()->showMessage(msg, 8000);
}

void MainWindow::rebuildCombos() {
    const Stats s = m_index.stats();
    m_ratingBox->blockSignals(true);
    m_ratingBox->clear();
    const QList<QPair<QString, QString>> ratings = {
        {QStringLiteral("all"), QStringLiteral("All")},
        {QStringLiteral("general"), QStringLiteral("General")},
        {QStringLiteral("sensitive"), QStringLiteral("Sensitive")},
        {QStringLiteral("questionable"), QStringLiteral("Questionable")},
        {QStringLiteral("explicit"), QStringLiteral("Explicit")},
    };
    for (const auto &[key, label] : ratings) {
        const int n = (key == QStringLiteral("all")) ? s.total : s.ratings.value(key);
        m_ratingBox->addItem(QStringLiteral("%1 (%2)").arg(label).arg(n), key);
    }
    const int ri = m_ratingBox->findData(m_rating);
    m_ratingBox->setCurrentIndex(qMax(0, ri));
    m_ratingBox->blockSignals(false);

    m_categoryBox->blockSignals(true);
    m_categoryBox->clear();
    m_categoryBox->addItem(QStringLiteral("All sources (%1)").arg(s.total), QString());
    QVector<QPair<QString, int>> cats;
    cats.reserve(s.categories.size());
    for (auto it = s.categories.begin(); it != s.categories.end(); ++it)
        cats.append({it.key(), it.value()});
    std::sort(cats.begin(), cats.end(),
              [](const auto &a, const auto &b) { return a.second > b.second; });
    for (const auto &[name, n] : cats)
        m_categoryBox->addItem(QStringLiteral("%1 (%2)").arg(name).arg(n), name);
    const int ci = m_categoryBox->findData(m_category);
    m_categoryBox->setCurrentIndex(qMax(0, ci));
    m_categoryBox->blockSignals(false);
}

void MainWindow::rebuildSidebar() {
    const Stats s = m_index.stats();
    rebuildTagTab(m_copyList, s.topCopyrights);
    rebuildTagTab(m_charList, s.topCharacters);
    rebuildTagTab(m_artistList, s.topArtists);
    rebuildTagTab(m_tagList, s.topGeneral);
    rebuildTagTab(m_metaList, s.topMeta);
}

void MainWindow::rebuildTagTab(QListWidget *list,
                               const QVector<QPair<QString, int>> &entries) {
    list->clear();
    for (const auto &[tag, n] : entries) {
        auto *it = new QListWidgetItem(QStringLiteral("%1  (%2)").arg(tag).arg(n));
        it->setData(Qt::UserRole, tag);
        if (m_activeTags.contains(tag)) {
            QFont f = it->font();
            f.setBold(true);
            it->setFont(f);
        }
        list->addItem(it);
    }
}

void MainWindow::applyFilter() {
    QStringList tokens = m_activeTags + m_searchQuery.split(QChar::Space, Qt::SkipEmptyParts);
    const QueryResult r =
        m_index.query(tokens, m_rating, m_category, m_sort, 1, INT_MAX);
    m_current = r.posts;
    m_model->clear();
    for (const Post &p : m_current) {
        auto *item = new QStandardItem();
        item->setData(QFileInfo(p.file).fileName(), Qt::DisplayRole);
        const QFileInfo fi(p.absFile);
        const QString key =
            p.absFile + u'|' + QString::number(fi.lastModified().toMSecsSinceEpoch());
        const auto cached = m_thumbCache.find(key);
        item->setData(cached != m_thumbCache.end() ? *cached : m_placeholder,
                      Qt::DecorationRole);
        item->setData(p.rating, Qt::UserRole + 1);
        QString kind;
        if (isVideoExt(p.ext))
            kind = QStringLiteral("▶");
        else if (p.ext == QStringLiteral("gif"))
            kind = QStringLiteral("GIF");
        item->setData(kind, Qt::UserRole + 2);
        item->setData(QStringLiteral("%1%2")
                          .arg(p.score > 0 ? QStringLiteral("+") : QStringLiteral(""))
                          .arg(p.score),
                      Qt::UserRole + 3);
        item->setData(key, Qt::UserRole + 4);
        item->setData(p.absFile, Qt::UserRole + 5);
        item->setData(p.id ? QStringLiteral("#%1").arg(p.id) : QStringLiteral("—"),
                      Qt::UserRole + 6);
        item->setEditable(false);
        item->setToolTip(p.file);
        m_model->appendRow(item);
    }
    updateResultLabel();
    requestVisibleThumbs();
}

void MainWindow::updateResultLabel() {
    int flagged = 0;
    for (const Post &p : m_current) {
        if (p.ratingInferred && p.rating == QStringLiteral("explicit"))
            flagged++;
    }
    m_resultLabel->setText(
        flagged > 0 ? QStringLiteral("%1 results · %2 auto-flagged explicit")
                          .arg(m_current.size())
                          .arg(flagged)
                    : QStringLiteral("%1 results").arg(m_current.size()));
    m_pills->clear();
    auto addPill = [this](const QString &text, const QString &tip, auto &&onClick) {
        auto *b = new QPushButton(text, this);
        b->setFlat(true);
        b->setCursor(Qt::PointingHandCursor);
        b->setToolTip(tip);
        b->setStyleSheet(QStringLiteral(
            "background:rgba(124,158,248,.15);border:1px solid rgba(124,158,248,.4);"
            "border-radius:4px;padding:2px 7px;color:#7c9ef8;"));
        connect(b, &QPushButton::clicked, this, onClick);
        m_pills->addWidget(b);
    };
    for (const QString &t : m_activeTags) {
        addPill(QStringLiteral("%1 ×").arg(t), QStringLiteral("Remove filter"),
                [this, t] {
                    m_activeTags.removeAll(t);
                    rebuildSidebar();
                    applyFilter();
                });
    }
    if (!m_searchQuery.trimmed().isEmpty()) {
        addPill(QStringLiteral("⌕“%1” ×").arg(m_searchQuery.trimmed()),
                QStringLiteral("Clear search"), [this] {
                    m_searchEdit->clear();
                    m_searchQuery.clear();
                    applyFilter();
                });
    }
    if (m_rating != QStringLiteral("all")) {
        addPill(QStringLiteral("[%1] ×").arg(m_rating), QStringLiteral("Clear rating"),
                [this] {
                    m_rating = QStringLiteral("all");
                    rebuildCombos();
                    applyFilter();
                });
    }
    if (!m_category.isEmpty()) {
        addPill(QStringLiteral("{%1} ×").arg(m_category),
                QStringLiteral("Clear source"), [this] {
                    m_category.clear();
                    rebuildCombos();
                    applyFilter();
                });
    }
    m_pillBar->setVisible(m_pills->count() > 0);
}

void MainWindow::requestVisibleThumbs() {
    if (m_current.isEmpty() || m_pages->currentIndex() != 0)
        return;
    const QRect vp = m_grid->viewport()->rect();
    const QModelIndex first = m_grid->indexAt(vp.topLeft());
    const QModelIndex last = m_grid->indexAt(vp.bottomRight());
    const int lo = first.isValid() ? first.row() : 0;
    const int hi = last.isValid() ? last.row() : m_current.size() - 1;
    const int overscan = 20;
    for (int row = qMax(0, lo - overscan); row <= qMin(hi + overscan, m_current.size() - 1);
         ++row) {
        const Post &p = m_current[row];
        const QFileInfo fi(p.absFile);
        if (!fi.exists())
            continue;
        const qint64 mtime = fi.lastModified().toMSecsSinceEpoch();
        const QString key = p.absFile + u'|' + QString::number(mtime);
        if (m_thumbCache.contains(key))
            continue;
        if (isVideoExt(p.ext))
            m_videoThumbs->request(key, p.absFile, mtime, fi.size(),
                                   m_settings.thumbLongSide);
        else
            m_thumbs->request(key, p.absFile, mtime, fi.size(),
                              m_settings.thumbLongSide);
    }
}

void MainWindow::onThumbnailReady(const QString &key, const QImage &image) {
    QPixmap pm = QPixmap::fromImage(image);
    if (pm.isNull())
        return;
    if (!m_thumbCache.contains(key)) {
        m_cacheOrder.append(key);
        if (m_cacheOrder.size() > 600)
            m_thumbCache.remove(m_cacheOrder.takeFirst());
    }
    m_thumbCache[key] = pm;
    for (int row = 0; row < m_model->rowCount(); ++row) {
        const QModelIndex idx = m_model->index(row, 0);
        if (m_model->data(idx, Qt::UserRole + 4).toString() == key)
            m_model->setData(idx, pm, Qt::DecorationRole);
    }
}

void MainWindow::onSearchEdited(const QString &text) {
    const QStringList parts = text.split(QChar::Space);
    QString last = parts.isEmpty() ? QString() : parts.last();
    QString bare = last.startsWith(u'-') ? last.mid(1) : last;
    if (bare.length() < 2)
        return;
    const auto sug = m_index.suggest(bare, 12);
    QStringList words;
    for (const auto &s : sug)
        words << s.tag;
    m_completeModel->setStringList(words);
    if (!words.isEmpty())
        m_completer->complete();
}

void MainWindow::onSearchReturn() {
    m_searchQuery = m_searchEdit->text();
    applyFilter();
}

void MainWindow::onCompleterActivated(const QString &text) {
    QString cur = m_searchEdit->text();
    int pos = cur.lastIndexOf(u' ');
    QString prefix = (pos >= 0) ? cur.left(pos + 1) : QString();
    QString last = (pos >= 0) ? cur.mid(pos + 1) : cur;
    const QString dash = last.startsWith(u'-') ? QStringLiteral("-") : QString();
    m_searchEdit->setText(prefix + dash + text + u' ');
}

void MainWindow::onRatingChanged() {
    m_rating = m_ratingBox->currentData().toString();
    applyFilter();
}

void MainWindow::onCategoryChanged() {
    m_category = m_categoryBox->currentData().toString();
    applyFilter();
}

void MainWindow::onAspectChanged() {
    const QVariantList v = m_aspectBox->currentData().toList();
    if (v.isEmpty()) {
        // Custom…
        bool ok = false;
        const QString s = QInputDialog::getText(
            this, QStringLiteral("Custom thumbnail ratio"),
            QStringLiteral("Aspect as W:H (e.g. 4:3):"), QLineEdit::Normal,
            QStringLiteral("%1:%2").arg(m_aspectNum).arg(m_aspectDen), &ok);
        if (ok) {
            const QStringList parts = s.split(u':');
            const int w = parts.value(0).toInt();
            const int h = parts.value(1).toInt();
            if (w > 0 && h > 0)
                setAspect(w, h);
            else
                setAspect(m_aspectNum, m_aspectDen); // restore box label
        } else {
            setAspect(m_aspectNum, m_aspectDen); // restore box label
        }
    } else {
        setAspect(v.value(0).toInt(), v.value(1).toInt());
    }
    applyFilter();
}

void MainWindow::setAspect(int num, int den) {
    m_aspectNum = qMax(1, num);
    m_aspectDen = qMax(1, den);
    m_settings.aspectNum = m_aspectNum;
    m_settings.aspectDen = m_aspectDen;
    saveSettings(m_settings);
    m_delegate->setAspect(m_aspectNum, m_aspectDen);
    m_thumbs->setAspect(m_aspectNum, m_aspectDen);
    m_videoThumbs->setAspect(m_aspectNum, m_aspectDen);
    // Memory cache holds old-aspect crops; disk cache has the right ones.
    m_thumbCache.clear();
    m_cacheOrder.clear();
    m_placeholder = QPixmap(kBoxW, (kBoxW * m_aspectDen) / m_aspectNum);
    m_placeholder.fill(QColor(0x1e, 0x21, 0x30));
    // Reflect a custom ratio in the combo's Custom entry.
    m_aspectBox->blockSignals(true);
    bool matched = false;
    for (int i = 0; i < m_aspectBox->count(); ++i) {
        const QVariantList v = m_aspectBox->itemData(i).toList();
        if (v.size() == 2 && v.value(0).toInt() == m_aspectNum &&
            v.value(1).toInt() == m_aspectDen) {
            m_aspectBox->setCurrentIndex(i);
            matched = true;
            break;
        }
    }
    if (!matched) {
        const int last = m_aspectBox->count() - 1; // Custom… entry
        m_aspectBox->setItemText(
            last, QStringLiteral("Custom %1:%2").arg(m_aspectNum).arg(m_aspectDen));
        m_aspectBox->setItemData(last, QVariantList{m_aspectNum, m_aspectDen});
        m_aspectBox->setCurrentIndex(last);
    }
    m_aspectBox->blockSignals(false);
}

void MainWindow::applyThemeFromSettings() {
    if (m_settings.themeMode == QStringLiteral("native")) {
        Theme::applyNative(*qApp);
    } else {
        Theme::applyDefault(*qApp, QColor(m_settings.accent));
        if (!m_settings.stylesheet.isEmpty())
            Theme::applyStylesheetFile(*qApp, m_settings.stylesheet);
    }
}

void MainWindow::openPreferences() {
    PrefsDialog dlg(m_settings, this);
    if (dlg.exec() != QDialog::Accepted)
        return;
    m_settings = dlg.settings();
    saveSettings(m_settings);
    setActiveExplicitTags(
        QSet<QString>(m_settings.explicitTags.begin(), m_settings.explicitTags.end()));
    m_fetcher->setGelbooruCredentials(m_settings.gelbooruUser, m_settings.gelbooruKey);
    applyThemeFromSettings();
    setAspect(m_settings.aspectNum, m_settings.aspectDen);
    const QString thumbDir = appCacheDir() + u'/' + QStringLiteral("thumbs");
    m_thumbs->setCacheDir(m_settings.thumbCache ? thumbDir : QString());
    m_videoThumbs->setCacheDir(m_settings.thumbCache ? thumbDir : QString());
    m_viewer->setPlaybackDefaults(m_settings.videoMuted, m_settings.videoLoop,
                                  m_settings.volume);
    m_viewer->setMaxSide(m_settings.viewerMaxSide);
    if (m_stage->currentIndex() == 1)
        startScan(m_root); // ratings may have changed -> rescan to apply
    else
        applyFilter();
}

void MainWindow::closeEvent(QCloseEvent *e) {
    m_settings.geometry = saveGeometry();
    if (m_sideSplit)
        m_settings.sideSplitter = m_sideSplit->saveState();
    saveSettings(m_settings);
    QMainWindow::closeEvent(e);
}

void MainWindow::onTagClicked() {
    auto *list = qobject_cast<QListWidget *>(sender());
    if (!list || !list->currentItem())
        return;
    const QString tag = list->currentItem()->data(Qt::UserRole).toString();
    if (tag.isEmpty())
        return;
    if (m_activeTags.contains(tag))
        m_activeTags.removeAll(tag);
    else
        m_activeTags.append(tag);
    rebuildSidebar();
    applyFilter();
}

void MainWindow::onGridActivated(const QModelIndex &index) {
    if (index.isValid())
        showViewer(index.row());
}

void MainWindow::showViewer(int row) {
    m_viewer->setPosts(m_current);
    m_viewer->showRow(row);
    m_pages->setCurrentIndex(1);
}

void MainWindow::backToGrid() {
    m_pages->setCurrentIndex(0);
    m_grid->setFocus();
    requestVisibleThumbs();
}
