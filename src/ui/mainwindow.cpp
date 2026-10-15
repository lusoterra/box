#include "mainwindow.h"
#include "appinfo.h"
#include "cache.h"
#include "cachejanitor.h"
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
#include "stray.h"
#include "straydialog.h"
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

#include <QApplication>
#include <QActionGroup>
#include <QCheckBox>
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
#include <QMenuBar>
#include <QMessageBox>
#include <QProcess>
#include <QMimeData>
#include <QPainter>
#include <QPushButton>
#include <QProgressBar>
#include <QProgressDialog>
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
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QtConcurrent>
#include <climits>
#include <utility>
#include <algorithm>

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

// Omnibar combos only answer the wheel once clicked (focused): hovering
// them while scrolling the grid must not flip filters by accident.
class WheelGuard : public QObject {
public:
    explicit WheelGuard(QObject *parent = nullptr) : QObject(parent) {}
    bool eventFilter(QObject *o, QEvent *e) override {
        if (e->type() == QEvent::Wheel) {
            const QWidget *w = qobject_cast<QWidget *>(o);
            if (w && !w->hasFocus())
                return true;
        }
        return false;
    }
};
} // namespace

namespace {
// Must mirror FavDb::pidFor/siteFor: remote keyed by site+id (id-less
// imports by site+URL hash), local by file hash. A downloaded favourite
// keeps its site label (rowToPost preserves it), so identity follows
// siteName whenever present - otherwise unfavouriting or deleting a
// downloaded post would miss its own database row.
std::pair<QString, qint64> favIdentity(const Post &p) {
    if (!p.siteName.isEmpty() || p.isRemote)
        return {p.siteName, FavDb::pidFor(p)};
    return {QStringLiteral("local"), static_cast<qint64>(qHash(p.absFile))};
}

// First-run tag seed: a fresh tag_types.db starts with the maintainer
// snapshot (top tags by usage, :/seed/tagtypes.tsv) so first scans
// categorize from cache instead of hundreds of rate-limited requests.
// INSERT OR IGNORE inside importSeed: established caches and user-fetched
// verdicts are never clobbered. Seedless builds just fetch live.
void seedTagTypesOnce(const QString &dbPath) {
    if (QFile::exists(dbPath))
        return;
    QFile seed(QStringLiteral(":/seed/tagtypes.tsv"));
    if (!seed.open(QIODevice::ReadOnly))
        return;
    TagTypeCache cache;
    if (!cache.open(dbPath))
        return;
    cache.importSeed(TagTypes::parseTagSeed(seed.readAll()));
}

bool blacklisted(const Post &p, const QSet<QString> &banned) {
    if (banned.isEmpty())
        return false;
    for (const QString &t : p.tags.split(QChar::Space, Qt::SkipEmptyParts)) {
        if (banned.contains(t))
            return true;
    }
    return false;
}

// Split search tokens ("-foo" excludes) for client-side filtering.
void splitIncExc(const QStringList &tokens, QStringList &inc, QStringList &exc) {
    for (const QString &tok : tokens) {
        if (tok.startsWith(u'-') && tok.size() > 1)
            exc << tok.mid(1);
        else if (!tok.isEmpty())
            inc << tok;
    }
}

bool isRatingToken(const QString &tok) {
    const QString low = tok.toLower();
    return low.startsWith(QStringLiteral("rating:")) ||
           low.startsWith(QStringLiteral("safety:"));
}

bool ratingMatches(const QString &postRating, const QString &tok) {
    QString want = tok.mid(tok.indexOf(u':') + 1).toLower();
    if (want == QStringLiteral("safe"))
        want = QStringLiteral("general"); // danbooru convention
    return postRating.compare(want, Qt::CaseInsensitive) == 0;
}

bool matchesTokens(const Post &p, const QStringList &inc, const QStringList &exc) {
    const QStringList pt = p.tags.split(QChar::Space, Qt::SkipEmptyParts);
    for (const QString &x : inc) {
        if (isRatingToken(x)) {
            if (!ratingMatches(p.rating, x))
                return false;
            continue;
        }
        if (!pt.contains(x))
            return false;
    }
    for (const QString &x : exc) {
        if (isRatingToken(x)) {
            if (ratingMatches(p.rating, x))
                return false;
            continue;
        }
        if (pt.contains(x))
            return false;
    }
    return true;
}

// `rating:X` / `safety:X` tokens filter on the post's rating field instead
// of literal tag containment (no post is literally tagged "rating:safe").
// Without this, default safe-search would empty every virtual-source merge.
// OR-group satisfaction for client-side (virtual-source) matching, using
// the same containment semantics as matchesTokens.
bool groupsMatch(const Post &p, const QList<QStringList> &groups) {
    if (groups.isEmpty())
        return true;
    const QStringList pt = p.tags.split(QChar::Space, Qt::SkipEmptyParts);
    return QueryOr::groupsMatch(QSet<QString>(pt.begin(), pt.end()), groups);
}

// Virtual multibooru sources: the local downloads folder and the
// favourites database behave like boorus in the picker and merge into
// browse tabs. '@'-prefixed so they can never collide with a booru key.
const QString kSrcDownloads = QStringLiteral("@downloads");
const QString kSrcFavourites = QStringLiteral("@favourites");

bool isVirtualSource(const QString &key) {
    return key == kSrcDownloads || key == kSrcFavourites;
}
} // namespace

// ── GalleryDelegate ──────────────────────────────────────────────

GalleryDelegate::GalleryDelegate(QObject *parent) : QStyledItemDelegate(parent) {}

void GalleryDelegate::setAspect(int num, int den) {
    m_num = qMax(1, num);
    m_den = qMax(1, den);
}

QSize GalleryDelegate::sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const {
    const int boxH = qMax(60, (m_boxW * m_den) / m_num);
    return {m_boxW + 12, boxH + 62};
}

void GalleryDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option,
                            const QModelIndex &index) const {
    painter->save();
    const int boxH = qMax(60, (m_boxW * m_den) / m_num);
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

    // AI badge (top-left over image, mirrors the kind badge).
    if (index.data(Qt::UserRole + 9).toBool()) {
        const QRect ab(imgR.left() + 4, imgR.top() + 4, 26, 18);
        painter->setPen(Qt::NoPen);
        painter->setBrush(QColor(0x7c, 0x4d, 0xff, 200));
        painter->drawRoundedRect(ab, 4, 4);
        painter->setPen(Qt::white);
        painter->drawText(ab, Qt::AlignCenter, QStringLiteral("AI"));
    }

    // Origin badge for remote posts (bottom-left over image).
    const QString origin = index.data(Qt::UserRole + 7).toString();
    if (!origin.isEmpty()) {
        const int ow = painter->fontMetrics().horizontalAdvance(origin) + 12;
        const QRect ob(imgR.left() + 4, imgR.bottom() - 22, ow, 18);
        painter->setPen(Qt::NoPen);
        QColor bg(0x3a, 0x7e, 0xc2, 220);
        const QColor configured(index.data(Qt::UserRole + 8).toString());
        if (configured.isValid()) {
            bg = configured;
            bg.setAlpha(220);
        }
        painter->setBrush(bg);
        painter->drawRoundedRect(ob, 4, 4);
        painter->setPen(Qt::white);
        painter->drawText(ob, Qt::AlignCenter, origin);
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

// Suggestion popup delegate: one color per tag bucket.
class SuggestDelegate : public QStyledItemDelegate {
public:
    explicit SuggestDelegate(QObject *parent = nullptr)
        : QStyledItemDelegate(parent) {}
    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override {
        QStyleOptionViewItem opt(option);
        initStyleOption(&opt, index);
        const QColor color =
            UiIcons::tagBucketColor(index.data(Qt::UserRole + 1).toString());
        opt.palette.setColor(QPalette::Text, color);
        opt.palette.setColor(QPalette::HighlightedText, color);
        QStyledItemDelegate::paint(painter, opt, index);
    }
};

// ── GridView ─────────────────────────────────────────────────────

GridView::GridView(QWidget *parent) : QListView(parent) {
    setDragEnabled(true);
    setDragDropMode(QAbstractItemView::DragOnly);
    setSelectionMode(QAbstractItemView::ExtendedSelection);
    // Per-pixel scrolling: in IconMode the bar's per-item steps count
    // items, not visual rows (several items share one row), so per-item
    // steps move a fraction of a row - or nowhere - per notch. Pixels
    // stay exact whatever the column count.
    setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
}

int GridView::rowPitch() const {
    int h = 0;
    if (model() && model()->rowCount() > 0)
        h = sizeHintForIndex(model()->index(0, 0)).height();
    return qMax(20, h + spacing());
}

void GridView::wheelEvent(QWheelEvent *e) {
    QScrollBar *bar = verticalScrollBar();
    const int notch = e->angleDelta().y();
    const int pixels = e->pixelDelta().y();
    if (!bar || (notch == 0 && pixels == 0) ||
        (e->modifiers() & Qt::ControlModifier)) {
        QListView::wheelEvent(e);
        return;
    }
    const int pitch = rowPitch();
    bar->setSingleStep(pitch);
    if (pixels != 0 && notch == 0) {
        // Precision trackpads report raw pixels.
        m_wheelPxAccum += -pixels;
    } else {
        // Notch (or smooth-wheel) ticks, scaled to whole visual rows;
        // fractional ticks and sub-1 speeds accumulate across events
        // instead of being dropped.
        m_wheelPxAccum += -double(notch) * m_scrollRows * pitch / 120.0;
    }
    const int step = int(m_wheelPxAccum);
    if (step != 0) {
        bar->setValue(bar->value() + step);
        m_wheelPxAccum -= step;
    }
    e->accept();
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
      m_fetcher(new TagTypeFetcher(this)),
      m_browseFetcher(new TagTypeFetcher(this)),
      m_imgCache(new ImageCache(this)),
      m_refresher(new PostRefresher(this)),
      m_downloads(new DownloadManager(this)) {
    m_settings = loadSettings();
    setActiveExplicitTags(
        QSet<QString>(m_settings.explicitTags.begin(), m_settings.explicitTags.end()));
    m_favicons = new FaviconCache(appCacheDir() + QStringLiteral("/favicons"), this);
    m_favicons->setCustomDir(effectiveDataDir(m_settings) +
                             QStringLiteral("/favicons"));
    WebApiFetcher::setProfilePath(effectiveDataDir(m_settings) +
                                  QStringLiteral("/webprofile"));
    RemoteSource::setWebFetcher([](const QUrl &url, int timeoutMs) {
        const WebApiFetcher::Result r =
            WebApiFetcher::fetchJsonBlocking(url, timeoutMs);
        return RemoteSource::WebFetchResult{r.http, r.body, r.error};
    });
    auto *webDownloads = new WebEngineDownloadBackend(this);
    m_downloads->setWebBackend(webDownloads);
    m_thumbs->setWebBackend(webDownloads);
    connect(m_favicons, &FaviconCache::ready, this, &MainWindow::onFaviconReady);
    RemoteSource::setConfiguredBoorus(m_settings.boorus);
    RemotePosts::setDisabledBuckets(m_settings.disabledBuckets);
    prefetchFavicons();
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
    // One-time layout moves: the cache tree used to nest twice
    // (~/.cache/booru-explorer/booru-explorer) and cache.db lived in the
    // data dir. Carry both over when the new spots are still empty.
    {
        const QString base = appCacheDir();
        for (const QString &sub :
             {QStringLiteral("thumbs"), QStringLiteral("roots"),
              QStringLiteral("favicons")}) {
            const QString dst = base + u'/' + sub;
            const QString src = base + QStringLiteral("/booru-explorer/") + sub;
            if (!QDir(dst).exists() && QDir(src).exists()) {
                QDir().mkpath(base);
                QDir().rename(src, dst);
            }
        }
        const QString newDb = base + QStringLiteral("/cache.db");
        const QString oldDb =
            effectiveDataDir(m_settings) + QStringLiteral("/cache.db");
        if (!QFile::exists(newDb) && QFile::exists(oldDb)) {
            QDir().mkpath(base);
            if (!QFile::rename(oldDb, newDb) && QFile::copy(oldDb, newDb))
                QFile::remove(oldDb); // cross-device fallback (tmpfs cache)
        }
    }
    CacheJanitor::configure(
        appCacheDir(), qint64(m_settings.cacheLimitMB) * 1024 * 1024);
    // Thumbnail/full disk caches live in the platform cache dir.
    const bool diskCache = m_settings.thumbCache;
    const QString thumbDir = appCacheDir() + u'/' + QStringLiteral("thumbs");
    m_thumbs->setCacheDir(diskCache ? thumbDir : QString());
    m_videoThumbs->setCacheDir(diskCache ? thumbDir : QString());

    auto *fileMenu = menuBar()->addMenu(tr("&File"));
    QMenu *openMenu = fileMenu->addMenu(tr("&Open"));
    openMenu->addAction(tr("&Folder…"), this, &MainWindow::openFolder);
    fileMenu->addAction(tr("Browse &Downloads"), this, [this] {
        addLocalTab(effectiveDownloadDir(m_settings));
    });
    fileMenu->addSeparator();
    fileMenu->addAction(tr("Import LoliSnatcher backup…"), this,
                        &MainWindow::importLosnBackup);
    fileMenu->addAction(tr("Download/Add Stray…"), this,
                        &MainWindow::openStrayDialog);
    fileMenu->addAction(tr("Update downloaded sidecars…"), this,
                        &MainWindow::updateDownloadSidecars);
    fileMenu->addSeparator();
    fileMenu->addAction(QStringLiteral("&Quit"), qApp, &QApplication::quit);

    auto *tabsMenu = menuBar()->addMenu(tr("&Tabs"));
    tabsMenu->addAction(tr("&Close Folder"), this, &MainWindow::showWelcome);
    QAction *favAct =
        tabsMenu->addAction(tr("&Favourites"), this, &MainWindow::openFavTab);
    favAct->setIcon(UiIcons::heart(UiIcons::favRed(), true));
    auto *reloadAct =
        tabsMenu->addAction(QStringLiteral("&Reload"), this, &MainWindow::reload);
    reloadAct->setShortcut(QKeySequence::Refresh); // F5

    auto *editMenu = menuBar()->addMenu(tr("&Edit"));
    auto *prefsAct = editMenu->addAction(tr("&Preferences…"), this,
                                         &MainWindow::openPreferences);
    prefsAct->setShortcut(QKeySequence(QStringLiteral("Ctrl+,")));
    editMenu->addAction(tr("Load Custom &Stylesheet…"), this,
                        &MainWindow::loadStylesheet);

    auto *helpMenu = menuBar()->addMenu(tr("&Help"));
    helpMenu->addAction(tr("&About booru-explorer"), this, &MainWindow::showAbout);
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
    m_searchEdit->setPlaceholderText(tr("Search tags… (-exclude, ~(a b) = either)"));
    m_searchEdit->setMinimumWidth(180);
    m_searchEdit->addAction(UiIcons::icon(QStringLiteral("search")),
                            QLineEdit::LeadingPosition);
    m_completeModel = new QStandardItemModel(this);
    m_completer = new QCompleter(m_completeModel, this);
    m_completer->setCaseSensitivity(Qt::CaseInsensitive);
    m_completer->setCompletionMode(QCompleter::PopupCompletion);
    m_completer->popup()->setItemDelegate(new SuggestDelegate(m_completer));
    m_searchEdit->setCompleter(m_completer);
    m_suggester = new TagSuggester(this);
    m_suggester->setLocalIndex(&m_index); // offline answers from your archive
    connect(m_suggester, &TagSuggester::finished, this,
            &MainWindow::onRemoteSuggest);
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
    m_sortBox->addItem(tr("Recently added"), QStringLiteral("added"));
    m_sortBox->addItem(tr("Newest"), QStringLiteral("id-desc"));
    m_sortBox->addItem(tr("Oldest"), QStringLiteral("id-asc"));
    m_sortBox->addItem(tr("Top Rated"), QStringLiteral("score-desc"));
    m_sortBox->addItem(tr("Low Rated"), QStringLiteral("score-asc"));
    m_sortBox->addItem(tr("Random"), QStringLiteral("random"));
    m_aspectBox = new QComboBox(this);
    m_aspectBox->addItem(QStringLiteral("1:1"), QVariantList{1, 1});
    m_aspectBox->addItem(QStringLiteral("4:3"), QVariantList{4, 3});
    m_aspectBox->addItem(QStringLiteral("3:2"), QVariantList{3, 2});
    m_aspectBox->addItem(QStringLiteral("16:9"), QVariantList{16, 9});
    m_aspectBox->addItem(QStringLiteral("3:4"), QVariantList{3, 4});
    m_aspectBox->addItem(tr("Custom…"), QVariantList{});
    connect(m_sortBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this] {
                m_sort = m_sortBox->currentData().toString();
                commitFilter();
            });
    connect(m_ratingBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &MainWindow::onRatingChanged);
    connect(m_categoryBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &MainWindow::onCategoryChanged);
    connect(m_aspectBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &MainWindow::onAspectChanged);
    m_sizeSlider = new QSlider(Qt::Horizontal, this);
    m_sizeSlider->setRange(96, 384);
    m_sizeSlider->setSingleStep(8);
    m_sizeSlider->setPageStep(32);
    m_sizeSlider->setFixedWidth(110);
    m_sizeSlider->setToolTip(tr("Post size in grid"));
    connect(m_sizeSlider, &QSlider::valueChanged, this,
            [this](int w) { setBoxW(w); });
    connect(m_sizeSlider, &QSlider::sliderReleased, this, [this] {
        m_settings.gridBoxW = m_delegate->boxW();
        saveSettings(m_settings);
    });

    auto *clearBtn = new QPushButton(tr("Clear"), this);
    auto *reloadBtn = new QPushButton(this);
    reloadBtn->setIcon(UiIcons::icon(QStringLiteral("refresh-cw")));
    reloadBtn->setIconSize(QSize(16, 16));
    reloadBtn->setToolTip(tr("Re-scan folder"));
    m_sitesBtn = new QPushButton(tr("Boorus…"), this);
    m_sitesBtn->setToolTip(tr("Choose boorus for this tab"));
    m_sitesMenu = new QMenu(this);
    rebuildSitesMenu();
    m_sitesBtn->setMenu(m_sitesMenu);
    connect(m_sitesMenu, &QMenu::triggered, this, &MainWindow::onSitesChanged);
    m_pageLabel = new QLabel(this);
    m_pageLabel->setStyleSheet(QStringLiteral("color:#4a5068;"));
    m_pageLabel->setToolTip(tr("Infinite scroll loads more as you reach the bottom"));
    m_selectBtn = new QPushButton(tr("Select"), this);
    m_selectBtn->setCheckable(true);
    m_selectBtn->setToolTip(tr("Select posts to download (clicks select instead of opening)"));
    connect(m_selectBtn, &QPushButton::toggled, this, [this](bool on) {
        m_selectMode = on;
        if (!on)
            m_grid->clearSelection();
        updateDownloadButton();
    });
    m_downloadSelBtn = new QPushButton(this);
    m_downloadSelBtn->setIcon(UiIcons::icon(QStringLiteral("download")));
    m_downloadSelBtn->setIconSize(QSize(15, 15));
    // Fixed footprint: the count text must not nudge its neighbours, and
    // the button never hides (it disables instead) so Select never
    // reshuffles the bar.
    m_downloadSelBtn->setMinimumWidth(
        m_downloadSelBtn->fontMetrics().horizontalAdvance(
            tr("Download (9999)")) +
        24);
    connect(m_downloadSelBtn, &QPushButton::clicked, this,
            &MainWindow::downloadSelected);
    connect(clearBtn, &QPushButton::clicked, this, [this] {
        m_searchEdit->clear();
        m_searchQuery.clear();
        m_activeTags.clear();
        if (isBrowse())
            commitSearch();
        else {
            rebuildSidebar();
            applyFilter();
        }
    });
    connect(reloadBtn, &QPushButton::clicked, this, &MainWindow::reload);
    // Filter combos ignore the wheel until clicked (same as Preferences).
    auto *wheelGuard = new WheelGuard(this);
    for (QComboBox *cb : {m_ratingBox, m_categoryBox, m_sortBox, m_aspectBox})
        cb->installEventFilter(wheelGuard);
    m_countLabel = new QLabel(this);
    m_countLabel->setStyleSheet(QStringLiteral("color:#4a5068;"));
    m_resultLabel = new QLabel(this);
    m_resultLabel->setStyleSheet(QStringLiteral("color:#4a5068;"));
    m_dotPage = new QLabel(QStringLiteral("·"), this);
    m_dotPage->setStyleSheet(QStringLiteral("color:#4a5068;"));
    m_dotCount = new QLabel(QStringLiteral("·"), this);
    m_dotCount->setStyleSheet(QStringLiteral("color:#4a5068;"));

    omniLay->addWidget(m_searchEdit, 1);
    omniLay->addWidget(m_ratingBox);
    omniLay->addWidget(m_categoryBox);
    omniLay->addWidget(m_sortBox);
    omniLay->addWidget(m_aspectBox);
    omniLay->addWidget(m_sitesBtn);
    omniLay->addWidget(m_selectBtn);
    omniLay->addWidget(m_downloadSelBtn);
    omniLay->addWidget(clearBtn);
    omniLay->addWidget(reloadBtn);
    omniLay->addStretch(1); // status cluster + zoom hug the right edge
    omniLay->addWidget(m_pageLabel);
    omniLay->addWidget(m_dotPage);
    omniLay->addWidget(m_resultLabel);
    omniLay->addWidget(m_dotCount);
    omniLay->addWidget(m_countLabel);
    omniLay->addWidget(m_sizeSlider); // grid zoom, far right

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
        {tr("COPYRIGHT"), m_copyList},
        {tr("CHARACTERS"), m_charList},
        {tr("ARTISTS"), m_artistList},
        {tr("TAGS"), m_tagList},
        {tr("META"), m_metaList},
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
    m_sideSplit->setChildrenCollapsible(false); // sections never snap shut
    if (!m_settings.sideSplitter.isEmpty())
        m_sideSplit->restoreState(m_settings.sideSplitter);
    connect(m_sideSplit, &QSplitter::splitterMoved, this, [this] {
        m_settings.sideSplitter = m_sideSplit->saveState();
        saveSettings(m_settings);
    });
    sideLay->addWidget(m_sideSplit, 1);
    side->setMinimumWidth(200);
    side->setMaximumWidth(460);

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
    // Double-click always opens; single-click opens unless Select mode.
    connect(m_grid, &QListView::doubleClicked, this,
            [this](const QModelIndex &i) {
                if (i.isValid())
                    showViewer(i.row());
            });
    connect(m_grid, &QListView::activated, this, &MainWindow::onGridActivated);
    connect(m_grid->selectionModel(), &QItemSelectionModel::selectionChanged,
            this, [this] { updateDownloadButton(); });
    m_grid->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_grid, &QListView::customContextMenuRequested, this,
            &MainWindow::showGridMenu);
    connect(m_thumbs, &ThumbLoader::thumbnailReady, this, &MainWindow::onThumbnailReady);
    connect(m_videoThumbs, &VideoThumbGrabber::thumbnailReady, this,
            &MainWindow::onThumbnailReady);
    connect(m_fetcher, &TagTypeFetcher::progress, this,
            &MainWindow::onClassifyProgress);
    connect(m_fetcher, &TagTypeFetcher::finished, this,
            &MainWindow::onClassifyFinished);
    connect(m_fetcher, &TagTypeFetcher::finished, this,
            &MainWindow::onManualFetchDone);
    connect(m_fetcher, &TagTypeFetcher::resolved, this,
            &MainWindow::onClassifyPartial);
    connect(m_fetcher, &TagTypeFetcher::gelbooruAuthFailed, this, [this] {
        statusBar()->showMessage(
            tr("Gelbooru rejected the API credentials - using "
                           "public sources (check User ID + key in Preferences)"),
            10000);
    });
    connect(m_fetcher, &TagTypeFetcher::backoff, this, [this] {
        statusBar()->showMessage(
            tr("Tag API rate-limited - pausing briefly…"), 6000);
    });
    connect(m_browseFetcher, &TagTypeFetcher::progress, this,
            &MainWindow::onClassifyProgress);
    connect(m_browseFetcher, &TagTypeFetcher::finished, this,
            &MainWindow::onBrowseClassifyFinished);
    connect(m_browseFetcher, &TagTypeFetcher::resolved, this,
            &MainWindow::onBrowseClassifyPartial);
    connect(m_browseFetcher, &TagTypeFetcher::backoff, this, [this] {
        statusBar()->showMessage(
            tr("Tag API rate-limited - pausing briefly…"), 6000);
    });
    connect(m_downloads, &DownloadManager::finished, this,
            [this](const DownloadSummary &s) {
                if (!m_favDb.isOpen())
                    return;
                const QString dest =
                    effectiveDownloadDir(m_settings);
                bool touched = false;
                for (const Post &p : s.downloaded) {
                    const auto [site, pid] = favIdentity(p);
                    // Strays follow their own toggle (default on): they have
                    // no home tab, so favouriting is how they stay findable.
                    const bool autoFav =
                        m_settings.favAutoFav ||
                        (m_settings.strayAutoFav &&
                         p.siteName == Stray::dirName());
                    if (autoFav && !m_favDb.isFavourite(site, pid)) {
                        m_favDb.add(p);
                        touched = true;
                    }
                    // Point favourites at the file just written (or an
                    // earlier copy): they open from disk afterwards.
                    if (m_favDb.isFavourite(site, pid)) {
                        m_favDb.setLocalPath(
                            site, pid,
                            dest + u'/' + p.siteName + u'/' +
                                RemotePosts::downloadFileName(p.siteName,
                                                              p.rawJson));
                        touched = true;
                    }
                }
                // New (or newly linked) favourites must show up in any
                // open favourites tab immediately, not after a manual ↺.
                if (touched)
                    refreshFavTabs();
            });
    m_favDb.open(effectiveDataDir(m_settings)); // best effort: store.db lives here
    seedTagTypesOnce(effectiveTagTypesDbPath(m_settings));
    m_viewer = new ViewerPage(this);
    connect(m_viewer, &ViewerPage::back, this, &MainWindow::backToGrid);
    m_viewer->setPlaybackDefaults(m_settings.videoMuted, m_settings.videoLoop,
                                  m_settings.volume);
    m_viewer->setMaxSide(m_settings.viewerMaxSide);
    applyBooruCreds();
    m_imgCache->setBaseDir(appCacheDir() + u'/' + QStringLiteral("full"));
    m_imgCache->setEnabled(m_settings.fullCache);
    m_viewer->setImageCache(m_imgCache);
    m_viewer->setThumbModel(m_model);
    m_viewer->setWebBackend(webDownloads);
    connect(m_viewer, &ViewerPage::downloadRequested, this,
            [this](const Post &p) { downloadPosts({p}); });
    connect(m_viewer, &ViewerPage::refreshRequested, this,
            &MainWindow::refreshPost);
    connect(m_refresher, &PostRefresher::refreshed, this,
            &MainWindow::onRefreshDone);
    connect(m_refresher, &PostRefresher::failed, this,
            &MainWindow::onRefreshFailed);
    connect(m_viewer, &ViewerPage::favouriteRequested, this,
            &MainWindow::toggleFavourite);
    connect(m_viewer, &ViewerPage::tagActivated, this, [this](const QString &tag) {
        if (!m_activeTags.contains(tag))
            m_activeTags.append(tag);
        if (isBrowse()) {
            commitSearch();
        } else {
            rebuildSidebar();
            applyFilter();
        }
        backToGrid();
    });
    m_pages->addWidget(m_grid);
    m_pages->addWidget(m_viewer);

    m_thumbTimer = new QTimer(this);
    m_thumbTimer->setSingleShot(true);
    m_thumbTimer->setInterval(120);
    connect(m_thumbTimer, &QTimer::timeout, this, &MainWindow::requestVisibleThumbs);
    // Waterfall sidebar: streaming verdicts flush here at most ~2/s while
    // a classify run is in flight (per-tag rebuilds would jank the UI).
    m_partialTimer = new QTimer(this);
    m_partialTimer->setSingleShot(true);
    m_partialTimer->setInterval(500);
    connect(m_partialTimer, &QTimer::timeout, this,
            &MainWindow::flushClassifyPartials);
    // Session persist coalescing (see saveTabs): bursts of tab switches
    // share one ini write instead of blocking each switch on disk.
    m_saveTimer = new QTimer(this);
    m_saveTimer->setSingleShot(true);
    m_saveTimer->setInterval(1500);
    connect(m_saveTimer, &QTimer::timeout, this, [this] {
        saveSettings(m_settings);
    });
    connect(m_grid->verticalScrollBar(), &QScrollBar::valueChanged, m_thumbTimer,
            static_cast<void (QTimer::*)()>(&QTimer::start));
    // Infinite scroll: near the bottom in a browse tab, load the next page.
    connect(m_grid->verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int v) {
        if (!isBrowse() || m_pages->currentIndex() != 0)
            return;
        auto *bar = m_grid->verticalScrollBar();
        if (v >= bar->maximum() - 400)
            loadNextPage();
    });
    connect(m_viewer, &ViewerPage::pastEnd, this, &MainWindow::onViewerPastEnd);

    auto *split = new QSplitter(this);
    split->addWidget(side);
    auto *center = new QWidget(this);
    auto *centerLay = new QVBoxLayout(center);
    centerLay->setContentsMargins(0, 0, 0, 0);
    // Tab bar + new-tab button (welcome lives in tab 0, not the attraction).
    auto *tabRow = new QHBoxLayout();
    tabRow->setContentsMargins(0, 0, 0, 0);
    tabRow->setSpacing(4);
    m_tabBar = new MarqueeTabBar(this);
    m_tabBar->setTabsClosable(true);
    m_tabBar->setMovable(true);
    m_tabBar->setExpanding(false);
    m_tabBar->addTab(QString());
    m_tabBar->setFullTitle(0, tr("Home"));
    m_tabBar->setTabIcon(0, UiIcons::icon(QStringLiteral("house")));
    m_tabBar->setTabToolTip(0, tr("Welcome"));
    // Home can't close: drop its X instead of showing a dead button.
    m_tabBar->setTabButton(0, QTabBar::RightSide, nullptr);
    m_tabBar->setTabButton(0, QTabBar::LeftSide, nullptr);
    connect(m_tabBar, &QTabBar::currentChanged, this, &MainWindow::onTabChanged);
    connect(m_tabBar, &QTabBar::tabCloseRequested, this, &MainWindow::onTabClosed);
    auto *plusBtn = new QPushButton(QStringLiteral("+"), this);
    plusBtn->setFixedWidth(32);
    plusBtn->setToolTip(tr("New tab"));
    m_plusBtn = plusBtn;
    connect(plusBtn, &QPushButton::clicked, this, &MainWindow::showPlusMenu);
    // Plus sits right after the last tab (not at the far right): the bar
    // keeps its own size, leftover space goes after the button.
    tabRow->addWidget(m_tabBar, 0);
    tabRow->addWidget(plusBtn);
    tabRow->addStretch(1);
    centerLay->addLayout(tabRow);
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
    split->setCollapsible(0, false); // sidebar keeps its width
    if (m_settings.geometry.isEmpty()) {
        // First run: roomy sidebar by default instead of a sliver.
        QTimer::singleShot(0, this, [this, split, side] {
            split->setSizes({280, qMax(400, width() - 280)});
            Q_UNUSED(side);
        });
    }

    // Stage: welcome page vs. browser.
    m_welcome = new WelcomePage(this);
    m_welcome->setMascot(m_settings.mascotPath);
    connect(m_welcome, &WelcomePage::openDialogRequested, this, &MainWindow::openFolder);
    connect(m_welcome, &WelcomePage::browseTabRequested, this, [this] {
        addRemoteTab(m_settings.browseSites, {});
    });
    connect(m_welcome, &WelcomePage::favTabRequested, this, &MainWindow::openFavTab);
    connect(m_welcome, &WelcomePage::resumeRequested, this,
            &MainWindow::openLastPlace);
    connect(m_welcome, &WelcomePage::downloadsRequested, this, [this] {
        addLocalTab(effectiveDownloadDir(m_settings));
    });
    connect(m_welcome, &WelcomePage::restoreToggled, this, [this](bool on) {
        m_settings.tabsDefault = on ? QStringLiteral("last") : QStringLiteral("welcome");
        saveSettings(m_settings);
    });
    m_welcome->setLastPlace(describeLastPlace()); // startup home state
    m_stage = new QStackedWidget(this);
    m_stage->addWidget(m_welcome);
    m_stage->addWidget(split);
    setCentralWidget(m_stage);
    m_scanBar = new CenterProgressBar(this);
    m_scanBar->setFixedWidth(180);
    m_scanBar->setTextVisible(true);
    m_scanBar->setVisible(false);
    statusBar()->addPermanentWidget(m_scanBar);
    // Feedbar: quiet download progress (click Downloads for details).
    m_dlBar = new CenterProgressBar(this);
    m_dlBar->setFixedWidth(140);
    m_dlBar->setTextVisible(true);
    m_dlBar->setVisible(false);
    m_dlBtn = new QPushButton(tr("Downloads"), this);
    m_dlBtn->setVisible(false);
    connect(m_dlBtn, &QPushButton::clicked, this, &MainWindow::showDlDialog);
    statusBar()->addPermanentWidget(m_dlBar);
    statusBar()->addPermanentWidget(m_dlBtn);
    m_dlHide = new QTimer(this);
    m_dlHide->setSingleShot(true);
    m_dlHide->setInterval(6000);
    connect(m_dlHide, &QTimer::timeout, this, [this] {
        m_dlBar->setVisible(false);
        m_dlBtn->setVisible(false);
    });
    connect(m_downloads, &DownloadManager::progress, this,
            [this](int done, int total, const QString &current) {
                m_dlHide->stop();
                m_dlBar->setRange(0, qMax(1, total));
                m_dlBar->setValue(done);
                m_dlBar->setToolTip(current);
                m_dlBar->setVisible(true);
                m_dlBtn->setVisible(true);
            });
    connect(m_downloads, &DownloadManager::finished, this,
            [this](const DownloadSummary &sum) {
                m_dlBar->setRange(0, 1);
                m_dlBar->setValue(1);
                QString doneMsg = tr("Downloads finished: %1 ok, %2 failed")
                                            .arg(sum.done)
                                            .arg(sum.failed);
                if (sum.skipped > 0)
                    doneMsg += tr(" (%1 already downloaded)").arg(sum.skipped);
                statusBar()->showMessage(doneMsg, 8000);
                m_dlHide->start();
            });
    statusBar()->showMessage(tr("Ready"));
    statusBar()->installEventFilter(this); // click errors -> response viewer

    m_placeholder = QPixmap(kBoxW, kBoxW);
    m_placeholder.fill(QColor(0x1e, 0x21, 0x30));

    setAspect(m_settings.aspectNum, m_settings.aspectDen);
    m_sizeSlider->blockSignals(true);
    m_sizeSlider->setValue(m_settings.gridBoxW);
    m_sizeSlider->blockSignals(false);
    setBoxW(m_settings.gridBoxW);
    m_grid->setScrollRows(m_settings.gridScrollRows);

    // Startup routing: explicit CLI root opens as a tab; otherwise the
    // Default tab setting wins (welcome home, last session, or a booru).
    const bool canResume = !m_settings.lastRoot.isEmpty() &&
                           QDir(m_settings.lastRoot).exists();
    const QString def = m_settings.tabsDefault;
        if (rootExplicit) {
        addLocalTab(m_root);
    } else if (def == QStringLiteral("last")) {
        // Full session restore (local + browse + favourites tabs); falls
        // back to the legacy last-root behavior when nothing was saved.
        if (!restoreTabs() && canResume)
            addLocalTab(m_settings.lastRoot);
    } else if (RemoteSource::siteFor(def) != nullptr) {
        addRemoteTab({def}, {});
    } else {
        m_tabBar->setCurrentIndex(0);
        showWelcome();
    }
}

// ── Tabs ─────────────────────────────────────────────────────

void MainWindow::saveCurrentTab() {
    if (m_curTab < 0 || m_curTab >= m_tabs.size())
        return;
    BrowserTab *t = m_tabs[m_curTab];
    t->activeTags = m_activeTags;
    t->searchQuery = m_searchQuery;
    t->rating = m_rating;
    t->category = m_category;
    t->sort = m_sort;
    if (!t->isRemote) {
        if (!t->isFavourites)
            t->localPosts = m_index.posts();
        // else: favourites list is DB-owned (re-listed on activation);
        // overwriting it here is what made new favourites vanish on tab
        // switches until the next manual refresh.
    } else
        t->browseMerged = m_browseMerged;
}

BrowserTab *MainWindow::currentTab() {
    if (m_curTab < 0 || m_curTab >= m_tabs.size())
        return nullptr;
    return m_tabs[m_curTab];
}

void MainWindow::saveTabs() {
    stageTabs();
    // Coalesce the ini write: switches come in bursts and nothing reads
    // the file back mid-session. Quit/restart flush immediately.
    if (m_saveTimer && !m_saveTimer->isActive())
        m_saveTimer->start();
}

void MainWindow::flushSavedTabs() {
    if (m_saveTimer)
        m_saveTimer->stop();
    stageTabs();
    saveSettings(m_settings);
}

void MainWindow::stageTabs() {
    saveCurrentTab(); // flush omnibar/filters into the structs first
    // Last place follows the live tab (home itself never overwrites it).
    if (BrowserTab *c = currentTab()) {
        m_settings.hasLastPlace = true;
        AppSettings::SavedTab &lp = m_settings.lastPlace;
        lp.kind = c->isFavourites ? QStringLiteral("favs")
                                  : (c->isRemote ? QStringLiteral("browse")
                                                 : QStringLiteral("local"));
        lp.root = c->isRemote ? QString() : c->root;
        lp.sites = c->isRemote ? c->sites : QStringList();
        lp.tags = c->activeTags;
        lp.query = c->searchQuery;
        lp.rating = c->rating;
        lp.category = c->category;
        lp.sort = c->sort;
    }
    m_settings.savedTabs.clear();
    for (BrowserTab *t : m_tabs) {
        if (!t->isRemote && !t->isFavourites)
            continue; // local archives never persist (library untouched)
        AppSettings::SavedTab st;
        if (t->isFavourites)
            st.kind = QStringLiteral("favs");
        else if (t->isRemote) {
            st.kind = QStringLiteral("browse");
            st.sites = t->sites;
        } else {
            st.kind = QStringLiteral("local");
            st.root = t->root;
        }
        st.tags = t->activeTags;
        st.query = t->searchQuery;
        st.rating = t->rating;
        st.category = t->category;
        st.sort = t->sort;
        m_settings.savedTabs.append(st);
    }
    m_settings.savedTabCurrent = qMax(0, m_curTab);
}

bool MainWindow::restoreTabs() {
        if (m_settings.savedTabs.isEmpty())
        return false;
    bool any = false;
    bool favsDone = false;
    m_tabBar->blockSignals(true);
    int current = 0;
    int n = 0;
    for (const AppSettings::SavedTab &st : m_settings.savedTabs) {
        if (n >= 25)
            break;
        if (st.kind == QStringLiteral("local")) {
            if (!QDir(st.root).exists())
                continue;
            addLocalTab(st.root);
        } else if (st.kind == QStringLiteral("browse")) {
            if (st.sites.isEmpty())
                continue;
            addRemoteTab(st.sites, st.tags);
        } else if (st.kind == QStringLiteral("favs")) {
            if (favsDone)
                continue; // single favourites tab
            openFavTab();
            favsDone = true;
        } else {
            continue;
        }
        BrowserTab *t = m_tabs.last();
        t->searchQuery = st.query;
        t->rating = st.rating.isEmpty() ? QStringLiteral("all") : st.rating;
        t->category = st.category;
        t->sort = st.sort.isEmpty() ? QStringLiteral("id-desc") : st.sort;
        updateTabTitle(m_tabs.size() - 1);
        if (n == qBound(0, m_settings.savedTabCurrent,
                        m_settings.savedTabs.size() - 1))
            current = m_tabs.size(); // tab-bar index (+1: tab 0 is welcome)
        n++;
        any = true;
    }
    m_tabBar->blockSignals(false);
    if (any) {
        // The batch above already moved the bar silently while blocked: if
        // it sits on the target, setCurrentIndex is a no-op and the tab
        // would never activate (stuck on welcome) - drive it directly.
        if (m_tabBar->currentIndex() == current)
            onTabChanged(current);
        else
            m_tabBar->setCurrentIndex(current); // fires once: scans/searches it
    }
    return any;
}

bool MainWindow::isBrowse() const {
    return m_curTab >= 0 && m_curTab < m_tabs.size() && m_tabs[m_curTab]->isRemote;
}

QIcon MainWindow::sitesIcon(const QStringList &sites) const {
    QList<QPixmap> marks;
    for (const QString &key : sites) {
        QPixmap pm;
        if (key == kSrcFavourites)
            pm = UiIcons::heart(UiIcons::favRed(), true).pixmap(16, 16);
        else if (key == kSrcDownloads)
            pm = UiIcons::icon(QStringLiteral("download")).pixmap(16, 16);
        else if (const BooruSite *site = RemoteSource::siteFor(key))
            pm = m_favicons->iconFor(key, site->baseUrl).pixmap(16, 16);
        if (!pm.isNull())
            marks << pm.scaled(16, 16, Qt::IgnoreAspectRatio,
                               Qt::SmoothTransformation);
        if (marks.size() >= 3)
            break;
    }
    if (marks.isEmpty())
        return {};
    if (marks.size() == 1)
        return QIcon(marks.first());
    QPixmap combo(marks.size() * 18 - 2, 16);
    combo.fill(Qt::transparent);
    QPainter painter(&combo);
    for (int i = 0; i < marks.size(); ++i)
        painter.drawPixmap(i * 18, 0, marks.at(i));
    return QIcon(combo);
}

void MainWindow::onFaviconReady(const QString &key, QIcon icon) {
    Q_UNUSED(icon);
    // Retitle browse tabs using this site (composite marks may have gained
    // an icon). The sites picker stays text-only for readability.
    for (int i = 0; i < m_tabs.size(); ++i) {
        if (m_tabs[i]->isRemote && m_tabs[i]->sites.contains(key))
            updateTabTitle(i);
    }
}

QStringList MainWindow::siteDisplayNames(const QStringList &sites) const {
    QStringList shown;
    for (const QString &key : sites) {
        if (key == kSrcFavourites)
            shown << tr("Favourites");
        else if (key == kSrcDownloads)
            shown << tr("Downloads");
        else {
            // Labels, not slug keys: custom boorus are keyed
            // "newbooru"/"newbooru2" but named by the user.
            const BooruSite *site = RemoteSource::siteFor(key);
            shown << (site ? site->label : key);
        }
    }
    return shown;
}

void MainWindow::updateDots() {
    const auto shown = [](QLabel *l) {
        return l && l->isVisible() && !l->text().trimmed().isEmpty();
    };
    // A dot only separates two shown strings — never dangles at an end
    // or next to an empty label.
    if (m_dotPage)
        m_dotPage->setVisible(shown(m_pageLabel) && shown(m_resultLabel));
    if (m_dotCount)
        m_dotCount->setVisible(shown(m_resultLabel) && shown(m_countLabel));
}

void MainWindow::updateTabTitle(int tabIdx) {
    if (tabIdx < 0 || tabIdx >= m_tabs.size())
        return;
    BrowserTab *t = m_tabs[tabIdx];
    QStringList tags = t->activeTags + t->searchQuery.split(QChar::Space, Qt::SkipEmptyParts);
    tags.removeDuplicates();
    if (t->isFavourites) {
        t->title = tr("Favourites");
        if (!tags.isEmpty())
            t->title += QStringLiteral(" · ") + tags.join(u' ');
        // Solid red heart icon carries the mark (a ♥ glyph in the title
        // would double it and can't be colored).
        m_tabBar->setTabIcon(tabIdx + 1,
                              UiIcons::heart(UiIcons::favRed(), true));
    } else if (t->isRemote) {
        const QStringList shown = siteDisplayNames(t->sites);
        t->title = tags.isEmpty() ? shown.join(QStringLiteral(" + ")) : tags.join(u' ');
        m_tabBar->setTabIcon(tabIdx + 1, sitesIcon(t->sites));
    } else {
        t->title = QDir(t->root).dirName();
        if (t->title.isEmpty())
            t->title = t->root;
        // The download folder is literally named "downloads": show it
        // like everywhere else instead of the raw dirname.
        if (t->title.compare(QStringLiteral("downloads"), Qt::CaseInsensitive) == 0)
            t->title = tr("Downloads");
        if (!tags.isEmpty())
            t->title += QStringLiteral(" · ") + tags.join(u' ');
        m_tabBar->setTabIcon(tabIdx + 1, UiIcons::icon(QStringLiteral("folder-open")));
    }
    m_tabBar->setFullTitle(tabIdx + 1, t->title);
}

void MainWindow::addLocalTab(const QString &root) {
    auto *t = new BrowserTab();
    t->isRemote = false;
    t->root = root;
    m_tabs.append(t);
    m_tabBar->addTab(QString());
    updateTabTitle(m_tabs.size() - 1);
    m_tabBar->setCurrentIndex(m_tabs.size()); // +1: tab 0 is welcome
    saveTabs();
}

void MainWindow::addRemoteTab(const QStringList &sites, const QStringList &tags) {
        auto *t = new BrowserTab();
    t->isRemote = true;
    t->sites = sites.isEmpty() ? m_settings.browseSites : sites;
    t->activeTags = tags;
    t->page = 1;
    m_tabs.append(t);
    m_tabBar->addTab(QString());
    updateTabTitle(m_tabs.size() - 1);
    m_tabBar->setCurrentIndex(m_tabs.size());
    saveTabs();
}

void MainWindow::onTabChanged(int index) {
        m_suggester->cancel();
    m_completer->popup()->hide();
    saveCurrentTab();
    if (index <= 0) {
        m_curTab = -1;
        showWelcome();
        saveTabs();
        return;
    }
    activateTab(index - 1);
    saveTabs();
}

void MainWindow::onTabClosed(int index) {
    if (index <= 0)
        return; // welcome tab stays
    const int tabIdx = index - 1;
    if (tabIdx < 0 || tabIdx >= m_tabs.size())
        return;
    const bool wasCurrent = (tabIdx == m_curTab);
    delete m_tabs.takeAt(tabIdx);
    m_tabBar->blockSignals(true);
    m_tabBar->removeTab(index);
    // Re-sync full titles (indexes shifted).
    for (int i = 0; i < m_tabs.size(); ++i)
        updateTabTitle(i);
    m_tabBar->blockSignals(false);
    if (wasCurrent) {
        if (m_tabs.isEmpty()) {
            // setCurrentIndex(0) would be a no-op here (Qt already selected
            // home while signals were blocked), leaving the stage stuck on
            // the closed tab: go home explicitly.
            showWelcome();
        } else {
            activateTab(qMin(tabIdx, m_tabs.size() - 1));
        }
    } else if (m_curTab > tabIdx) {
        m_curTab--;
    }
    saveTabs();
}

void MainWindow::activateTab(int tabIdx) {
    if (tabIdx < 0 || tabIdx >= m_tabs.size())
        return;
    backToGrid();
    m_curTab = tabIdx;
    BrowserTab *t = m_tabs[tabIdx];
    m_tabBar->blockSignals(true);
    m_tabBar->setCurrentIndex(tabIdx + 1);
    m_tabBar->blockSignals(false);
    m_stage->setCurrentIndex(1);
    // Restore shared UI from the tab.
    m_activeTags = t->activeTags;
    m_searchQuery = t->searchQuery;
    m_searchEdit->setText(t->searchQuery);
    m_rating = t->rating;
    m_category = t->category;
    m_sort = t->sort;
    m_sortBox->blockSignals(true);
    m_sortBox->setCurrentIndex(qMax(0, m_sortBox->findData(m_sort)));
    m_sortBox->blockSignals(false);
    setBrowseUi(t->isRemote);
    // Skeleton first: the tab bar + stage paint NOW; the DB/scan/filter
    // rebuild runs on the next event-loop turn so no switch ever blocks
    // on it. Superseded switches abort in finishActivateTab.
    m_current.clear();
    m_model->clear();
    m_scanBar->setRange(0, 0);
    m_scanBar->setVisible(true);
    QTimer::singleShot(0, this, [this, tabIdx] {
        if (tabIdx != m_curTab)
            return; // a newer switch already took over
        finishActivateTab(tabIdx);
    });
}

void MainWindow::finishActivateTab(int tabIdx) {
    if (tabIdx < 0 || tabIdx >= m_tabs.size() || tabIdx != m_curTab)
        return;
    BrowserTab *t = m_tabs[tabIdx];
    if (t->isRemote && !t->isFavourites)
        maybeClassifyBrowse(t);
    if (t->isFavourites) {
        relinkFavourites(); // point rows at files already on disk (once)
        t->localPosts = m_favDb.list();
        setWindowTitle(QStringLiteral("booru-explorer - ♥ Favourites"));
        m_rating = t->rating;
        m_category = t->category;
        m_sort = t->sort;
        m_sortBox->blockSignals(true);
        m_sortBox->setCurrentIndex(qMax(0, m_sortBox->findData(m_sort)));
        m_sortBox->blockSignals(false);
        commitFilter(); // rebuilds rating/category combos, filters, titles
        m_scanBar->setVisible(false); // skeleton done (no async work here)
        return;
    }
    if (t->isRemote) {
        syncSitesMenu();
        m_pageLabel->setText(tr("Page %1").arg(t->page));
        if (t->browseMerged.isEmpty())
            doBrowseSearch();
        else {
            m_browseMerged = t->browseMerged;
            applyBrowseFilter();
            m_scanBar->setVisible(false); // skeleton done
        }
    } else {
        m_root = t->root;
        setWindowTitle(QStringLiteral("booru-explorer - %1").arg(m_root));
        if (t->localPosts.isEmpty()) {
            startScan(m_root);
        } else {
            m_index.setPosts(t->localPosts);
            rebuildCombos();
            rebuildSidebar();
            applyFilter();
            m_scanBar->setVisible(false); // skeleton done
        }
    }
    updateTabTitle(tabIdx);
}

// ── Browse ───────────────────────────────────────────────────

void MainWindow::setBrowseUi(bool browse) {
    const BrowserTab *t = currentTab();
    const bool favs = t && t->isFavourites;
    // Favourites get the same select/download tools as browse tabs
    // (mass-download what you starred); local archives stay clean.
    const bool downloadable = browse || favs;
    m_categoryBox->setVisible(!browse);
    m_sitesBtn->setVisible(browse);
    m_pageLabel->setVisible(browse);
    // The archive-total count label is meaningless on browse/fav tabs
    // (they have their own result line).
    m_countLabel->setVisible(!browse && !favs);
    updateDots(); // separators follow their neighbours, not the mode
    m_selectBtn->setVisible(downloadable);
    updateDownloadButton(); // owns m_downloadSelBtn visibility + state
    // The archive-total count label is meaningless on browse/fav tabs
    // (they have their own result line).
    m_countLabel->setVisible(!browse && !favs);
}

RemoteSource *MainWindow::sourceFor(const QString &key) {
    auto it = m_sources.find(key);
    RemoteSource *src = (it != m_sources.end()) ? it.value() : nullptr;
    const BooruSite *site = RemoteSource::siteFor(key);
    if (!src) {
        if (!site)
            return nullptr;
        src = new RemoteSource(*site, this);
        connect(src, &RemoteSource::searchFinished, this,
                &MainWindow::onRemoteFinished);
        m_sources.insert(key, src);
    }
    // Registry may have changed under us (prefs edits): resync the base URL
    // every call so edited instances take effect without a restart.
    if (site)
        src->setBaseUrl(site->baseUrl);
    return src;
}

SiteCreds MainWindow::credsFor(const QString &key) const {
    SiteCreds c;
    const BooruSite *site = RemoteSource::siteFor(key);
    if (site) {
        c.user = site->user;
        c.secret = site->secret;
    }
    return c;
}

void MainWindow::prefetchFavicons() {
    if (!m_favicons)
        return;
    for (const BooruSite &b : m_settings.boorus)
        m_favicons->prefetch(b.key, b.baseUrl);
}

void MainWindow::refreshFavicons() {
    if (!m_favicons)
        return;
    m_favicons->refreshAll();
    prefetchFavicons();
    rebuildSitesMenu();
    for (int i = 0; i < m_tabs.size(); ++i)
        updateTabTitle(i);
    statusBar()->showMessage(tr("Refreshing favicons…"), 4000);
}

void MainWindow::rebuildSitesMenu() {
    m_sitesMenu->clear();
    for (const BooruSite &s : m_settings.boorus) {
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
            [this, prog, batch](int updated, int failed, int skipped) {
                batch->deleteLater();
                prog->close();
                statusBar()->showMessage(
                    tr("Sidecars updated: %1 ok, %2 failed, %3 skipped")
                        .arg(updated)
                        .arg(failed)
                        .arg(skipped),
                    8000);
                // The current grid may show refreshed posts: rescan cheaply.
                if (updated > 0 && m_curTab >= 0 && !m_tabs[m_curTab]->isRemote &&
                    !m_tabs[m_curTab]->isFavourites)
                    startScan(m_root);
            });
    connect(prog, &QProgressDialog::canceled, batch, &SidecarBatch::cancel);
    connect(batch, &SidecarBatch::finished, batch, &SidecarBatch::deleteLater);
    batch->start(dest);
    if (!batch->isActive()) {
        // Nothing collectible: start() already emitted finished.
        prog->close();
        batch->deleteLater();
    }
}

void MainWindow::showPlusMenu() {
    QMenu menu(this);
    QAction *homeAct = menu.addAction(
        UiIcons::icon(QStringLiteral("house")), tr("Home"));
    connect(homeAct, &QAction::triggered, this, [this] { showWelcome(); });
    QAction *favAct = menu.addAction(
        UiIcons::heart(UiIcons::favRed(), true),
        tr("Favourites"));
    connect(favAct, &QAction::triggered, this, &MainWindow::openFavTab);
    QAction *dlAct = menu.addAction(UiIcons::icon(QStringLiteral("download")),
                                    tr("Downloads"));
    connect(dlAct, &QAction::triggered, this,
            [this] { addLocalTab(effectiveDownloadDir(m_settings)); });
    menu.addSeparator();
    // One click = one tab. Merging stays available per-tab through the
    // "Boorus…" menu, which checks any set of sources into the open tab.
    for (const BooruSite &s : m_settings.boorus) {
        QAction *a = menu.addAction(m_favicons->iconFor(s.key, s.baseUrl),
                                    s.label);
        a->setData(s.key);
        connect(a, &QAction::triggered, this, [this, a] {
            addRemoteTab({a->data().toString()}, {});
        });
    }
    menu.exec(m_plusBtn->mapToGlobal(QPoint(0, m_plusBtn->height())));
}

void MainWindow::syncSitesMenu() {
    BrowserTab *t = currentTab();
    const QStringList active = t ? t->sites : m_settings.browseSites;
    for (QAction *a : m_sitesMenu->actions())
        a->setChecked(active.contains(a->data().toString()));
}

void MainWindow::onSitesChanged() {
    BrowserTab *t = currentTab();
    if (!t || !t->isRemote)
        return;
    QStringList sites;
    for (QAction *a : m_sitesMenu->actions()) {
        if (a->isChecked())
            sites << a->data().toString();
    }
    if (sites.isEmpty()) {
        syncSitesMenu(); // refuse to uncheck everything
        return;
    }
    t->sites = sites;
    t->page = 1;
    updateTabTitle(m_curTab);
    doBrowseSearch(false);
}

// Infinite scroll: fetch the next page and append (throttled against 429s).
void MainWindow::loadNextPage() {
    BrowserTab *t = currentTab();
    if (!t || !t->isRemote || m_loadingPage)
        return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - m_lastPageLoadMs < 1500)
        return; // cooldown: never hammer the APIs
    m_lastPageLoadMs = now;
    t->page++;
    m_pageLabel->setText(tr("Page %1 · loading…").arg(t->page));
    statusBar()->showMessage(tr("Loading more…"), 4000);
    m_scanBar->setRange(0, 0); // busy animation until the page lands
    m_scanBar->setVisible(true);
    doBrowseSearch(true);
}

void MainWindow::pumpSiteSearch(const QString &key) {
    auto it = m_pendingSites.find(key);
    if (it == m_pendingSites.end() || it->queriesLeft.isEmpty())
        return;
    BrowserTab *t = m_searchTab;
    RemoteSource *src = m_sources.value(key, nullptr);
    if (!t || !src)
        return;
    const QStringList tokens = it->queriesLeft.takeFirst();
    src->setCreds(credsFor(key));
    m_pendingSearches++;
    src->search(RemotePosts::translateSearchTags(tokens, it->engine), t->page,
                m_settings.browseLimit);
}

void MainWindow::doBrowseSearch(bool append) {
    BrowserTab *t = currentTab();
    if (!t || !t->isRemote)
        return;
    clearStatusProbe(); // a new search invalidates the old error details
    m_appendResults = append;
    m_loadingPage = true;
    // Instant feedback: new cards render now (bucket colors refine when
    // results land), with the busy bar until the page arrives.
    updateResultLabel();
    if (!append) {
        m_scanBar->setRange(0, 0);
        m_scanBar->setVisible(true);
    }
    // OR-groups split off BEFORE space-splitting (they contain spaces).
    // Engines without a native OR get one search per alternative below.
    const QueryOr::ParsedQuery pq = QueryOr::parse(t->searchQuery);
    QStringList andBase = t->activeTags + pq.andTokens;
    for (const QString &b : m_settings.blacklist) {
        if (!b.isEmpty())
            andBase << QStringLiteral("-") + b;
    }
    // Rating is server-side in browse mode (translated per engine at
    // pump time): a change refetches like a new search instead of
    // filtering the loaded pages.
    if (t->rating != QStringLiteral("all"))
        andBase << QStringLiteral("rating:") + t->rating;
    // Per-site AND set: the entry's own defaults on top of the query.
    auto siteAndBase = [&](const BooruSite *cfg) {
        QStringList tags = andBase;
        tags += cfg->defaultTags.split(QChar::Space, Qt::SkipEmptyParts);
        return tags;
    };
    QStringList sites = t->sites.isEmpty() ? m_settings.browseSites : t->sites;
    if (sites.isEmpty())
        return;
    for (auto it = m_sources.begin(); it != m_sources.end(); ++it)
        it.value()->cancel();
    m_pendingSearches = 0;
    m_pendingSites.clear();
    m_searchTab = t;
    statusBar()->showMessage(
        tr("Searching %1…").arg(siteDisplayNames(sites).join(QStringLiteral(", "))));
    QStringList errors;
    QStringList skipped;
    for (const QString &key : sites) {
        if (isVirtualSource(key))
            continue; // merged locally in onRemoteFinished, no fetch
        const BooruSite *cfg = RemoteSource::siteFor(key);
        if (!cfg) {
            errors << tr("%1: unknown site").arg(key);
            continue;
        }
        if (cfg->baseUrl.isEmpty()) {
            skipped << key; // self-hosted, no instance URL configured
            continue;
        }
        if (!sourceFor(key)) {
            errors << tr("%1: unknown site").arg(key);
            continue;
        }
        // One queued search per OR combination; pumped one at a time so
        // every engine answers the same semantics (most have no native OR).
        // Expansion is per site: entry defaults change the AND set (and
        // the combination count) per entry.
        bool over = false;
        const QList<QStringList> expansions = QueryOr::expand(
            {siteAndBase(cfg), pq.groups}, QueryOr::kMaxCombos, over);
        if (over) {
            errors << tr("%1: too many OR combinations - narrow the groups")
                            .arg(cfg->label);
            continue;
        }
        if (expansions.isEmpty()) {
            errors << tr("%1: empty OR group - nothing to search")
                            .arg(cfg->label);
            continue;
        }
        PendingSite ps;
        ps.engine = cfg->engine;
        ps.queriesLeft = expansions;
        m_pendingSites.insert(key, ps);
        pumpSiteSearch(key);
    }
    if (m_pendingSearches == 0) {
        m_loadingPage = false;
        m_scanBar->setVisible(false);
        if (!skipped.isEmpty())
            errors << tr("%1 skipped (no instance URL)")
                           .arg(siteDisplayNames(skipped).join(", "));
        if (!errors.isEmpty())
            statusBar()->showMessage(errors.join(QStringLiteral(" · ")), 8000);
        else
            onRemoteFinished();
        return;
    }
}

void MainWindow::onRemoteFinished() {
    // Attribute this completion to its site: accumulate posts across the
    // site's queued OR-alternatives, then pump the next one (if any).
    if (RemoteSource *done = qobject_cast<RemoteSource *>(sender())) {
        for (auto it = m_sources.begin(); it != m_sources.end(); ++it) {
            if (it.value() != done)
                continue;
            const QString key = it.key();
            if (!m_pendingSites.contains(key))
                break;
            PendingSite &ps = m_pendingSites[key];
            if (!done->lastError().isEmpty() && ps.error.isEmpty()) {
                const BooruSite *cfg = RemoteSource::siteFor(key);
                QString oneLine;
                if (done->lastHttp() == 401 && cfg &&
                    cfg->engine == BooruEngine::Gelbooru)
                    oneLine =
                        tr("%1: login required (add an API key in "
                           "Preferences \u2192 Booru manager)")
                            .arg(cfg->label);
                else {
                    const BooruSite *errSite = RemoteSource::siteFor(key);
                    oneLine = tr("%1: %2").arg(errSite ? errSite->label : key,
                                               done->lastError());
                }
                ps.error = oneLine;
                ps.probe = oneLine + u'\n' + RemotePosts::probeReportText(
                    RemotePosts::redactSecrets(
                        done->lastUrl().toString(QUrl::FullyEncoded)),
                    done->lastHttp(), done->lastBody());
            }
            ps.posts += done->lastPosts();
            if (!ps.queriesLeft.isEmpty())
                pumpSiteSearch(key);
            break;
        }
    }
    if (--m_pendingSearches > 0) {
        statusBar()->showMessage(
            tr("Searching… %1 left").arg(m_pendingSearches));
        return;
    }
    // All done: merge in tab site order.
    BrowserTab *t = m_searchTab;
    m_searchTab = nullptr;
    if (!t)
        return;
    QVector<Post> merged;
    QStringList errors;
    QStringList probeSections; // raw request/response per failing site
    int hiddenWalled = 0;
    const QStringList sites = t->sites.isEmpty() ? m_settings.browseSites : t->sites;
    // Virtual sources contribute local posts (tag-filtered like the server
    // filters remote ones); page 1 only, so infinite scroll never doubles
    // them. Rating/blacklist filtering happens for everything below.
    const bool wantVirtual = !m_appendResults || t->page <= 1;
    QStringList vinc, vexc;
    QList<QStringList> vgroups;
    if (wantVirtual) {
        // OR-groups split off before space-splitting (they contain spaces).
        const QueryOr::ParsedQuery vpq = QueryOr::parse(t->searchQuery);
        vgroups = vpq.groups;
        QStringList vtokens = t->activeTags + vpq.andTokens;
        splitIncExc(vtokens, vinc, vexc);
    }
    for (const QString &key : sites) {
        if (isVirtualSource(key)) {
            if (!wantVirtual)
                continue;
            QVector<Post> local;
            if (key == kSrcFavourites) {
                if (m_favDb.isOpen())
                    local = m_favDb.list();
            } else {
                const QString dir = effectiveDownloadDir(m_settings);
                if (QDir(dir).exists())
                    local = Scanner::scan(dir, nullptr, m_settings.indexCache);
            }
            for (const Post &p : local) {
                if (matchesTokens(p, vinc, vexc) && groupsMatch(p, vgroups))
                    merged.append(p);
            }
            continue;
        }
        const auto bit = m_pendingSites.constFind(key);
        if (bit == m_pendingSites.constEnd())
            continue; // unknown site (error recorded at dispatch)
        if (!bit->error.isEmpty()) {
            errors << bit->error;
            probeSections << bit->probe;
        }
        for (const Post &p : bit->posts) {
            // Login-walled entries carry no viewable media: hide them up
            // front instead of showing dead grid items.
            if (p.fileUrl.isEmpty() && p.sampleUrl.isEmpty()) {
                hiddenWalled++;
                continue;
            }
            // Stamp the configured entry behind this result: badges (and
            // anything else per-entry) resolve exactly, even when several
            // entries share one engine family.
            Post stamped = p;
            stamped.siteKey = key;
            // Per-host buckets fold into general here (meme hosts): the
            // parse already applied the engine-family grid; the entry's
            // own set applies on top. Membership is unchanged (moved, not
            // removed), so p.tags stays valid.
            auto foldField = [&](QString &field, const char *bucket) {
                const QString b = QString::fromLatin1(bucket);
                if (!field.isEmpty() &&
                    RemotePosts::foldedBucketFor(stamped, b) ==
                        QStringLiteral("general")) {
                    stamped.tagsGeneral = stamped.tagsGeneral.isEmpty()
                                              ? field
                                              : stamped.tagsGeneral + u' ' + field;
                    field.clear();
                }
            };
            foldField(stamped.tagsArtist, "artist");
            foldField(stamped.tagsCharacter, "character");
            foldField(stamped.tagsCopyright, "copyright");
            foldField(stamped.tagsMeta, "meta");
            merged.append(stamped);
        }
    }
    m_pendingSites.clear();
    m_loadingPage = false;
    m_scanBar->setVisible(false); // page landed (classify re-shows if needed)
    // Dedupe by site+id across OR-alternatives (and, on append, against
    // the posts already showing).
    QSet<QString> seen;
    if (m_appendResults && t->page > 1) {
        // Infinite scroll: append, skipping duplicates.
        for (const Post &p : t->browseMerged)
            seen.insert(p.siteName + u'#' + QString::number(p.id));
        for (const Post &p : merged) {
            const QString k = p.siteName + u'#' + QString::number(p.id);
            if (!seen.contains(k)) {
                seen.insert(k);
                t->browseMerged.append(p);
            }
        }
    } else {
        t->browseMerged.clear();
        for (const Post &p : merged) {
            const QString k = p.siteName + u'#' + QString::number(p.id);
            if (!seen.contains(k)) {
                seen.insert(k);
                t->browseMerged.append(p);
            }
        }
    }
    m_appendResults = false;
    if (t != currentTab()) {
        return; // user switched tabs: stash only, no UI touch
    }
    m_browseMerged = t->browseMerged;
    if (!errors.isEmpty()) {
        statusBar()->showMessage(errors.join(QStringLiteral(" · ")), 10000);
        setStatusProbe(tr("Search errors"),
                       probeSections.join(QStringLiteral("\n\n")));
    } else {
        clearStatusProbe();
        QString msg = tr("%1 results · page %2")
                          .arg(t->browseMerged.size())
                          .arg(t->page);
        if (hiddenWalled > 0)
            msg += tr(" · %1 login-walled hidden").arg(hiddenWalled);
        statusBar()->showMessage(msg, 5000);
    }
    m_pageLabel->setText(tr("Page %1").arg(t->page));
    applyBrowseFilter();
    updateTabTitle(m_curTab);
    maybeClassifyBrowse(t);
    if (m_advanceOnLoad) {
        // Viewer asked past the end: continue there with the fresh items.
        m_advanceOnLoad = false;
        if (m_advanceRow < m_current.size())
            showViewer(m_advanceRow);
        else
            statusBar()->showMessage(tr("No more posts"), 4000);
    }
}

void MainWindow::applyBrowseFilter() {
    BrowserTab *t = currentTab();
    QVector<Post> posts = m_browseMerged;
    const QSet<QString> banned(m_settings.blacklist.begin(),
                               m_settings.blacklist.end());
    if (!banned.isEmpty()) {
        // Safety net (server-side -exclusion is in the query already).
        posts.erase(std::remove_if(posts.begin(), posts.end(),
                                   [&](const Post &p) { return blacklisted(p, banned); }),
                    posts.end());
    }
    if (t && t->rating != QStringLiteral("all")) {
        posts.erase(std::remove_if(posts.begin(), posts.end(),
                                   [&](const Post &p) { return p.rating != t->rating; }),
                    posts.end());
    }
    sortPosts(posts, t ? t->sort : m_sort);
    m_current = posts;
    fillModel();
    rebuildBrowseSidebar();
    updateResultLabel();
    updateTabTitle(m_curTab);
}

void MainWindow::sortPosts(QVector<Post> &posts, const QString &sort) {
    if (sort == QStringLiteral("id-asc"))
        std::sort(posts.begin(), posts.end(),
                  [](const Post &a, const Post &b) { return a.id < b.id; });
    else if (sort == QStringLiteral("score-desc"))
        std::sort(posts.begin(), posts.end(),
                  [](const Post &a, const Post &b) { return a.score > b.score; });
    else if (sort == QStringLiteral("score-asc"))
        std::sort(posts.begin(), posts.end(),
                  [](const Post &a, const Post &b) { return a.score < b.score; });
    else if (sort == QStringLiteral("random"))
        std::shuffle(posts.begin(), posts.end(), *QRandomGenerator::global());
    // "added" and id-desc default: keep incoming order (DB newest-first for
    // favourites, API newest-first for browse).
}

void MainWindow::rebuildBrowseSidebar() {
    // Top tags of the current page, bucketed properly (remote Danbooru
    // posts carry splits; blobs stay general).
    // Sections follow the tab's booru(s): a bucket hidden in EVERY
    // selected host hides its whole section (union in multibooru).
    static const QList<QPair<QString, int>> sections = {
        {QStringLiteral("copyright"), 0}, {QStringLiteral("character"), 1},
        {QStringLiteral("artist"), 2},    {QStringLiteral("general"), 3},
        {QStringLiteral("meta"), 4},
    };
    QSet<QString> hidden;
    if (const BrowserTab *t = currentTab(); t && t->isRemote && !t->sites.isEmpty()) {
        for (const auto &[bucket, idx] : sections) {
            Q_UNUSED(idx);
            if (bucket == QStringLiteral("general"))
                continue;
            bool offEverywhere = true;
            bool anyHost = false;
            for (const QString &key : t->sites) {
                if (isVirtualSource(key))
                    continue; // local mirrors: buckets always on
                const BooruSite *entry = RemoteSource::siteFor(key);
                if (!entry)
                    continue;
                anyHost = true;
                if (!entry->disabledBuckets.contains(bucket) &&
                    !RemotePosts::bucketDisabled(entry->engine, bucket)) {
                    offEverywhere = false;
                    break;
                }
            }
            if (anyHost && offEverywhere)
                hidden.insert(bucket);
        }
    }
    for (const auto &[bucket, idx] : sections)
        m_sideSplit->widget(idx)->setVisible(!hidden.contains(bucket));
    QHash<QString, QHash<QString, int>> counts = {
        {QStringLiteral("artist"), {}},   {QStringLiteral("character"), {}},
        {QStringLiteral("copyright"), {}}, {QStringLiteral("general"), {}},
        {QStringLiteral("meta"), {}},
    };
    for (const Post &p : m_current) {
        for (const QString &t : p.tagsArtist.split(QChar::Space, Qt::SkipEmptyParts))
            counts[QStringLiteral("artist")][t]++;
        for (const QString &t : p.tagsCharacter.split(QChar::Space, Qt::SkipEmptyParts))
            counts[QStringLiteral("character")][t]++;
        for (const QString &t : p.tagsCopyright.split(QChar::Space, Qt::SkipEmptyParts))
            counts[QStringLiteral("copyright")][t]++;
        for (const QString &t : p.tagsGeneral.split(QChar::Space, Qt::SkipEmptyParts))
            counts[QStringLiteral("general")][t]++;
        for (const QString &t : p.tagsMeta.split(QChar::Space, Qt::SkipEmptyParts))
            counts[QStringLiteral("meta")][t]++;
    }
    auto top = [](const QHash<QString, int> &m) {
        QVector<QPair<QString, int>> v;
        for (auto it = m.begin(); it != m.end(); ++it)
            v.append({it.key(), it.value()});
        std::sort(v.begin(), v.end(),
                  [](const auto &a, const auto &b) { return a.second > b.second; });
        if (v.size() > 30)
            v.resize(30);
        return v;
    };
    rebuildTagTab(m_copyList, hidden.contains(QStringLiteral("copyright"))
                                   ? QVector<QPair<QString, int>>()
                                   : top(counts[QStringLiteral("copyright")]));
    rebuildTagTab(m_charList, hidden.contains(QStringLiteral("character"))
                                   ? QVector<QPair<QString, int>>()
                                   : top(counts[QStringLiteral("character")]));
    rebuildTagTab(m_artistList, hidden.contains(QStringLiteral("artist"))
                                    ? QVector<QPair<QString, int>>()
                                    : top(counts[QStringLiteral("artist")]));
    rebuildTagTab(m_tagList, top(counts[QStringLiteral("general")]));
    rebuildTagTab(m_metaList, hidden.contains(QStringLiteral("meta"))
                                  ? QVector<QPair<QString, int>>()
                                  : top(counts[QStringLiteral("meta")]));
}

void MainWindow::onViewerPastEnd() {
    if (!isBrowse() || m_loadingPage)
        return;
    // Fetch more, then step into the first new post.
    m_advanceRow = m_current.size();
    m_advanceOnLoad = true;
    loadNextPage();
    if (!m_loadingPage) {
        // Nothing to load (cooldown); drop the request.
        m_advanceOnLoad = false;
    }
}

void MainWindow::downloadPosts(const QVector<Post> &posts) {
    QVector<Post> remote;
    for (const Post &p : posts) {
        if (p.isRemote && !p.fileUrl.isEmpty())
            remote.append(p);
    }
    if (remote.isEmpty()) {
        statusBar()->showMessage(
            tr("Nothing downloadable (login-walled entries have no file URL)"),
            6000);
        return;
    }
    const QString dest = effectiveDownloadDir(m_settings);
    const QList<DownloadJob> jobs = DownloadManager::plan(remote, dest);
    if (jobs.isEmpty()) {
        statusBar()->showMessage(tr("Nothing to download"), 4000);
        return;
    }
    m_downloads->enqueue(jobs); // no popup: progress lives in the feedbar
}

void MainWindow::showDlDialog() {
    const QString dest = effectiveDownloadDir(m_settings);
    if (!m_dlDialog) {
        m_dlDialog = new DownloadsDialog(m_downloads, dest, this);
        m_dlDialog->setAttribute(Qt::WA_DeleteOnClose, false);
        connect(m_downloads, &DownloadManager::progress, m_dlDialog,
                &DownloadsDialog::onProgress);
        connect(m_downloads, &DownloadManager::fileStarted, m_dlDialog,
                &DownloadsDialog::onFileStarted);
        connect(m_downloads, &DownloadManager::fileFinished, m_dlDialog,
                &DownloadsDialog::onFileFinished);
        connect(m_downloads, &DownloadManager::fileSkipped, m_dlDialog,
                &DownloadsDialog::onFileSkipped);
        connect(m_downloads, &DownloadManager::fileRenamed, m_dlDialog,
                &DownloadsDialog::onFileRenamed);
        connect(m_downloads, &DownloadManager::finished, m_dlDialog,
                &DownloadsDialog::onFinished);
        connect(m_dlDialog, &DownloadsDialog::browseRequested, this,
                &MainWindow::openPath);
    }
    m_dlDialog->show();
    m_dlDialog->raise();
}


void MainWindow::toggleFavourite(const Post &post) {
    if (!m_favDb.isOpen()) {
        statusBar()->showMessage(tr("Favourites unavailable"), 4000);
        return;
    }
    const auto [site, pid] = favIdentity(post);
    if (m_favDb.isFavourite(site, pid)) {
        m_favDb.remove(site, pid);
        m_viewer->setFaved(false);
        maybeDeleteUnfavFile(post);
        statusBar()->showMessage(tr("Removed from favourites"), 3000);
    } else {
        if (!m_favDb.add(post)) {
            statusBar()->showMessage(tr("Could not favourite"), 4000);
            return;
        }
        m_viewer->setFaved(true);
        statusBar()->showMessage(tr("♥ Favourited"), 3000);
        if (m_settings.favAutoDownload && post.isRemote && !post.fileUrl.isEmpty())
            downloadPosts({post});
    }
    // Refresh every open favourites tab (not just the current one).
    refreshFavTabs();
}

// Delete the downloaded copy behind an unfavourite when the option is on.
// Only files inside the download folder qualify: library archives are
// never touched by unfavouriting.
void MainWindow::maybeDeleteUnfavFile(const Post &post) {
    if (!m_settings.unfavDeletesFile || post.absFile.isEmpty())
        return;
    const QString dlRoot =
        QDir(effectiveDownloadDir(m_settings)).canonicalPath();
    const QString abs = QFileInfo(post.absFile).canonicalFilePath();
    if (!dlRoot.isEmpty() && !abs.isEmpty() &&
        (abs == dlRoot || abs.startsWith(dlRoot + u'/'))) {
        if (QFile::remove(abs))
            statusBar()->showMessage(tr("Deleted %1").arg(abs), 6000);
    }
}

void MainWindow::showGridMenu(const QPoint &pos) {
    const QModelIndex clicked = m_grid->indexAt(pos);
    if (!clicked.isValid() || clicked.row() < 0 ||
        clicked.row() >= m_current.size())
        return;
    // Targets: the selection when it contains the click, else the click.
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
    if (targets.isEmpty() || !m_favDb.isOpen())
        return;
    bool allFav = true;
    bool anyRemote = false;
    QStringList anyFiles;
    for (const Post &p : targets) {
        const auto [site, pid] = favIdentity(p);
        if (!m_favDb.isFavourite(site, pid))
            allFav = false;
        if (p.isRemote && !p.fileUrl.isEmpty())
            anyRemote = true;
        if (!p.absFile.isEmpty() && QFileInfo::exists(p.absFile))
            anyFiles << QFileInfo(p.absFile).canonicalFilePath();
    }
    anyFiles.removeDuplicates();
    QMenu menu(this);
    if (allFav) {
        QAction *unfav = menu.addAction(tr("♡ Unfavourite"));
        connect(unfav, &QAction::triggered, this, [this, targets] {
            for (const Post &p : targets) {
                const auto [site, pid] = favIdentity(p);
                if (m_favDb.remove(site, pid))
                    maybeDeleteUnfavFile(p);
            }
            refreshFavTabs();
            statusBar()->showMessage(tr("Removed %1 from favourites")
                                         .arg(targets.size()),
                                     4000);
        });
    } else {
        QAction *fav = menu.addAction(tr("♥ Favourite"));
        connect(fav, &QAction::triggered, this, [this, targets] {
            int added = 0;
            for (const Post &p : targets) {
                if (m_favDb.add(p))
                    added++;
            }
            refreshFavTabs();
            statusBar()->showMessage(
                tr("♥ Favourited %1").arg(added), 4000);
        });
    }
    QAction *dl = menu.addAction(tr("⭳ Download"));
    dl->setEnabled(anyRemote);
    connect(dl, &QAction::triggered, this, [this, targets] {
        QVector<Post> remote;
        for (const Post &p : targets) {
            if (p.isRemote && !p.fileUrl.isEmpty())
                remote.append(p);
        }
        downloadPosts(remote);
    });
    // Tag editing is a stray-only affair: exactly one stray file, so the
    // dialog prefills from its sidecar. Everything else keeps the menu.
    if (targets.size() == 1 && Stray::isStrayPost(targets.first())) {
        QAction *edit = menu.addAction(tr("Edit tags…"));
        connect(edit, &QAction::triggered, this,
                [this, post = targets.first()] { editStrayTags(post); });
    }
    // Delete is a disk-only affair unless "deleting unfavourites" is on:
    // it removes the files (plus our download sidecars) and leaves the
    // favourites alone. Unfavourite above is the mirror: it drops the DB
    // entries and only deletes files when "unfavouriting deletes" is on.
    // Remote-only posts have no file, so they stay Delete-disabled.
    // Local archives are read-only: no Delete there at all (the downloads
    // folder browsed as a tab still allows it).
    bool archiveTab = false;
    if (BrowserTab *t = currentTab();
        t && !t->isRemote && !t->isFavourites) {
        const QString dlRoot =
            QDir(effectiveDownloadDir(m_settings)).canonicalPath();
        const QString tabRoot = QDir(t->root).canonicalPath();
        archiveTab =
            tabRoot.isEmpty() || dlRoot.isEmpty() || tabRoot != dlRoot;
    }
    QAction *del = menu.addAction(tr("Delete from disk…"));
    del->setEnabled(!anyFiles.isEmpty() && !archiveTab);
    connect(del, &QAction::triggered, this, [this, targets, anyFiles] {
        if (anyFiles.isEmpty())
            return;
        const bool unfav = m_settings.deleteUnfavs;
        // Regenerability per file: a kept favourite with a source link
        // comes back via re-download; anything else is gone for good.
        auto sidecarUrl = [](const QString &absFile) {
            const QFileInfo fi(absFile);
            const QString dir = fi.absolutePath();
            const QString stem = fi.completeBaseName();
            for (const QString &sp :
                 {dir + u'/' + stem + QStringLiteral(".json"),
                  absFile + QStringLiteral(".json")}) {
                QFile f(sp);
                if (!f.open(QIODevice::ReadOnly))
                    continue;
                QJsonParseError err;
                const QJsonDocument doc =
                    QJsonDocument::fromJson(f.readAll(), &err);
                if (err.error == QJsonParseError::NoError && doc.isObject()) {
                    const QString u = doc.object()
                                          .value(QStringLiteral("file_url"))
                                          .toString();
                    if (!u.isEmpty())
                        return u;
                }
            }
            return QString();
        };
        // canonical path -> stays favourited but cannot regenerate
        // (warned), vs sidecar must survive (staying favourited AND
        // regenerable: the sidecar is the re-download manifest).
        QSet<QString> doomed, keepSidecar;
        for (const Post &p : targets) {
            if (p.absFile.isEmpty())
                continue;
            const QString abs =
                QFileInfo(p.absFile).canonicalFilePath();
            if (!anyFiles.contains(abs))
                continue;
            const auto [site, pid] = favIdentity(p);
            if (!m_favDb.isFavourite(site, pid) || unfav)
                continue; // gone from favourites too: nothing to warn about
            const bool regen =
                p.isRemote ? !p.fileUrl.isEmpty() : !sidecarUrl(abs).isEmpty();
            (regen ? keepSidecar : doomed).insert(abs);
        }
        QString question =
            unfav ? tr("Permanently delete %1 file(s) from disk and remove "
                       "them from favourites?")
                        .arg(anyFiles.size())
                  : tr("Permanently delete %1 file(s) from disk?")
                        .arg(anyFiles.size());
        if (!doomed.isEmpty())
            question += tr("\n\n%1 favourited file(s) have no source link "
                           "and cannot be re-downloaded.")
                            .arg(doomed.size());
        if (QMessageBox::question(this, tr("Delete"), question,
                                  QMessageBox::Yes | QMessageBox::No,
                                  QMessageBox::No) != QMessageBox::Yes)
            return;
        const QString dlRoot =
            QDir(effectiveDownloadDir(m_settings)).canonicalPath();
        int gone = 0;
        for (const QString &f : anyFiles) {
            if (QFile::remove(f)) {
                gone++;
                // Our download sidecar shares the stem (<stem>.json):
                // remove it so no orphan is left behind — unless it is a
                // kept re-download manifest (see above). Archive sidecars
                // are the user's metadata and stay untouched.
                if (!dlRoot.isEmpty() &&
                    (f == dlRoot || f.startsWith(dlRoot + u'/')) &&
                    !keepSidecar.contains(f)) {
                    const QFileInfo fi(f);
                    const QString sidecar = fi.absolutePath() + u'/' +
                                            fi.completeBaseName() +
                                            QStringLiteral(".json");
                    if (sidecar != f)
                        QFile::remove(sidecar);
                }
            }
        }
        int unfaved = 0;
        if (m_settings.deleteUnfavs) {
            for (const Post &p : targets) {
                const auto [site, pid] = favIdentity(p);
                if (m_favDb.remove(site, pid))
                    unfaved++;
            }
        }
        refreshFavTabs();
        if (m_curTab >= 0 && !currentTab()->isRemote &&
            !currentTab()->isFavourites)
            startScan(m_root); // archive changed: rescan
        statusBar()->showMessage(
            unfaved > 0
                ? tr("Deleted %1 file(s), removed %2 from favourites")
                      .arg(gone)
                      .arg(unfaved)
                : tr("Deleted %1 file(s)").arg(gone),
            6000);
    });
    menu.exec(m_grid->viewport()->mapToGlobal(pos));
}

void MainWindow::relinkFavourites() {
    if (!m_favDb.isOpen())
        return;
    const QVector<FavDb::Unlinked> missing = m_favDb.unlinked();
    if (missing.isEmpty())
        return;
    // Search roots: current download folder plus the legacy default,
    // where files downloaded by older versions live.
    QStringList roots;
    roots << effectiveDownloadDir(m_settings);
    const QString legacy = QStandardPaths::writableLocation(
                               QStandardPaths::PicturesLocation) +
                           QStringLiteral("/booru-explorer");
    if (QDir(legacy).exists() && !roots.contains(legacy))
        roots << legacy;
    // One listing per site folder (not one per missing row): fresh imports
    // can leave hundreds unlinked, and entryList per row stalls tab switches.
    QHash<QString, QHash<QString, QString>> bySite; // site -> stem -> abs path
    for (const QString &root : roots) {
        const QDir rootDir(root);
        if (!rootDir.exists())
            continue;
        for (const QString &siteDir :
             rootDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            const QDir dir(rootDir.absoluteFilePath(siteDir));
            auto &stems = bySite[siteDir];
            for (const QFileInfo &fi : dir.entryInfoList(QDir::Files)) {
                const QString stem = fi.completeBaseName();
                // Any prefixed stem qualifies (<Site>_<md5> and the
                // underscore-leading _Stray_<md5> alike).
                if (stem.contains(u'_') && !stems.contains(stem))
                    stems.insert(stem, fi.absoluteFilePath());
            }
        }
    }
    QHash<QPair<QString, qint64>, QString> found;
    for (const FavDb::Unlinked &u : missing) {
        const QString md5 = LosnImport::md5FromFileUrl(u.fileUrl);
        if (md5.isEmpty())
            continue;
        const auto it = bySite.constFind(u.site);
        if (it == bySite.constEnd())
            continue;
        // <Label>_<md5>.<any ext>
        for (auto s = it->begin(); s != it->end(); ++s) {
            if (s.key().endsWith(u'_' + md5)) {
                found.insert({u.site, u.id}, s.value());
                break;
            }
        }
    }
    const int n = m_favDb.setLocalPaths(found);
    // Regeneration: favourited strays whose file is gone but whose source
    // URL survives re-download into place (kept favourites only). Once
    // per session per row; failures simply retry next launch. Never
    // preempts an active download batch.
    QVector<Post> regen;
    if (m_downloads && !m_downloads->isActive()) {
        for (const FavDb::Unlinked &u : missing) {
            if (u.site != Stray::dirName() || u.fileUrl.isEmpty() ||
                !RemotePosts::isHttpUrl(u.fileUrl))
                continue;
            if (found.contains({u.site, u.id}))
                continue;
            const QString key =
                u.site + u'#' + QString::number(u.id) + u'|' + u.fileUrl;
            if (m_regenTried.contains(key))
                continue;
            m_regenTried.insert(key);
            Post p;
            p.isRemote = true;
            p.siteName = u.site;
            p.id = u.id;
            p.fileUrl = u.fileUrl;
            p.ext = u.ext;
            QJsonObject raw;
            raw[QStringLiteral("stray")] = true;
            raw[QStringLiteral("tags")] = u.tags;
            if (!u.ext.isEmpty())
                raw[QStringLiteral("file_ext")] = u.ext;
            p.rawJson = raw;
            regen.append(p);
        }
    }
    if (!regen.isEmpty()) {
        downloadPosts(regen);
        statusBar()->showMessage(
            tr("Re-downloading %1 missing stray file(s)…").arg(regen.size()),
            8000);
    } else if (n > 0) {
        statusBar()->showMessage(
            tr("Linked %1 favourites to files on disk.").arg(n), 6000);
    }
}

void MainWindow::openStrayDialog() {
    auto *dlg = new StrayDialog(m_settings.strayAutoFav, this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    connect(dlg, &StrayDialog::autoFavChanged, this, [this](bool on) {
        m_settings.strayAutoFav = on;
        saveSettings(m_settings);
    });
    connect(dlg, &StrayDialog::downloadRequested, this,
            [this](const QStringList &urls) {
                QVector<Post> posts;
                for (const QString &u : urls) {
                    const QString t = u.trimmed();
                    if (t.isEmpty() || !RemotePosts::isHttpUrl(t))
                        continue;
                    Post p;
                    p.isRemote = true;
                    p.siteName = Stray::dirName();
                    p.fileUrl = t;
                    QJsonObject raw;
                    raw[QStringLiteral("stray")] = true;
                    raw[QStringLiteral("tags")] = QString();
                    p.rawJson = raw;
                    posts.append(p);
                }
                if (!posts.isEmpty())
                    downloadPosts(posts);
            });
    connect(dlg, &StrayDialog::importRequested, this,
            &MainWindow::importStrayFiles);
    dlg->show();
    dlg->raise();
}

void MainWindow::importStrayFiles(const QStringList &paths) {
    const QString dest = effectiveDownloadDir(m_settings) + u'/' +
                         Stray::dirName();
    QDir().mkpath(dest);
    // Hash + copy off the UI thread; big videos must not freeze it.
    // Result: (failed sources, imported absolute paths).
    auto *watcher =
        new QFutureWatcher<QPair<QStringList, QStringList>>(this);
    connect(watcher, &QFutureWatcher<QPair<QStringList, QStringList>>::finished,
            this, [this, watcher, dest] {
                const auto [errors, imported] = watcher->result();
                watcher->deleteLater();
                const int ok = imported.size();
                int faved = 0;
                if (ok > 0 && m_settings.strayAutoFav && m_favDb.isOpen()) {
                    QVector<Post> favs;
                    favs.reserve(ok);
                    for (const QString &abs : imported) {
                        Post p;
                        p.absFile = abs;
                        p.file = abs;
                        p.ext = QFileInfo(abs).suffix().toLower();
                        // Sidecar holds the hand-written tags (empty until
                        // the user edits them); snapshot them into the row.
                        QFile jf(QFileInfo(abs).absolutePath() + u'/' +
                                 QFileInfo(abs).completeBaseName() +
                                 QStringLiteral(".json"));
                        if (jf.open(QIODevice::ReadOnly)) {
                            QJsonParseError err;
                            const QJsonDocument doc = QJsonDocument::fromJson(
                                jf.readAll(), &err);
                            if (err.error == QJsonParseError::NoError &&
                                doc.isObject())
                                MetaParser::parse(doc.object(), p);
                            p.absFile = abs;
                            p.file = abs;
                        }
                        favs.append(p);
                    }
                    faved = m_favDb.addMany(favs);
                    if (faved > 0)
                        refreshFavTabs();
                }
                if (!errors.isEmpty()) {
                    statusBar()->showMessage(
                        tr("Imported %1 file(s), %2 failed").arg(ok).arg(
                            errors.size()),
                        8000);
                } else if (faved > 0) {
                    statusBar()->showMessage(
                        tr("Imported %1 file(s) to _Stray, favourited %2")
                            .arg(ok)
                            .arg(faved),
                        6000);
                } else {
                    statusBar()->showMessage(
                        tr("Imported %1 file(s) to _Stray").arg(ok), 6000);
                }
                BrowserTab *t = currentTab();
                if (t && !t->isRemote && !t->isFavourites &&
                    (t->root == dest ||
                     t->root.startsWith(dest + u'/')))
                    startScan(m_root);
            });
    watcher->setFuture(QtConcurrent::run([paths, dest] {
        QStringList errors;
        QStringList imported;
        for (const QString &src : paths) {
            const QFileInfo fi(src);
            if (!fi.isFile() || !fi.isReadable()) {
                errors << src;
                continue;
            }
            const QString md5 = fileContentMd5(src);
            QString ext = fi.suffix().toLower();
            if (ext.isEmpty())
                ext = QStringLiteral("bin");
            const QString name = Stray::dirName() + u'_' + md5 + u'.' + ext;
            if (md5.isEmpty() || QFile::exists(dest + u'/' + name)) {
                if (md5.isEmpty())
                    errors << src;
                continue; // already imported (or unreadable)
            }
            if (!QFile::copy(src, dest + u'/' + name)) {
                errors << src;
                continue;
            }
            QJsonObject sidecar;
            sidecar[QStringLiteral("tags")] = QString();
            sidecar[QStringLiteral("file_ext")] = ext;
            sidecar[QStringLiteral("md5")] = md5;
            sidecar[QStringLiteral("stray")] = true;
            const QString stem = name.left(name.lastIndexOf(u'.'));
            QFile jf(dest + u'/' + stem + QStringLiteral(".json"));
            if (jf.open(QIODevice::WriteOnly | QIODevice::Truncate))
                jf.write(QJsonDocument(sidecar).toJson(
                    QJsonDocument::Indented));
            imported << dest + u'/' + name;
        }
        return qMakePair(errors, imported);
    }));
}

void MainWindow::editStrayTags(const Post &post) {
    const QFileInfo fi(post.absFile);
    const QString sidecar = fi.absolutePath() + u'/' +
                            fi.completeBaseName() +
                            QStringLiteral(".json");
    QString tags;
    QJsonObject obj;
    {
        QFile f(sidecar);
        if (f.open(QIODevice::ReadOnly)) {
            QJsonParseError err;
            const QJsonDocument doc =
                QJsonDocument::fromJson(f.readAll(), &err);
            if (err.error == QJsonParseError::NoError && doc.isObject())
                obj = doc.object();
            tags = obj.value(QStringLiteral("tags")).toString();
        }
    }
    if (tags.isEmpty())
        tags = post.tags;
    StrayTagDialog dlg(fi.fileName(), tags, this);
    if (dlg.exec() != QDialog::Accepted)
        return;
    // Normalize like the rest of the tag universe: lowercase, deduped.
    // (The classifier, search and sidebar all key on lowercase.)
    QStringList kept;
    for (const QString &t :
         dlg.tags().toLower().split(QChar::Space, Qt::SkipEmptyParts)) {
        if (!kept.contains(t))
            kept << t;
    }
    obj[QStringLiteral("tags")] = kept.join(u' ');
    obj[QStringLiteral("stray")] = true;
    {
        QFile f(sidecar);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            statusBar()->showMessage(
                tr("Cannot write %1").arg(sidecar), 6000);
            return;
        }
        f.write(
            QJsonDocument(obj).toJson(QJsonDocument::Indented));
    }
    // The scan cache keys on the MEDIA file (mtime+size): a sidecar-only
    // edit would read stale, so the tags never became searchable. Touch
    // the media so future rescans reparse the sidecar too.
    QFile touch(post.absFile);
    if (touch.open(QIODevice::ReadWrite))
        touch.setFileTime(QDateTime::currentDateTime(),
                          QFileDevice::FileModificationTime);
    // Apply now (no rescan flash): reparse what we just wrote, patch the
    // live index, and run the classifier so the new tags categorize
    // immediately (manual overrides always, online fetch when enabled).
    Post updated = post;
    if (MetaParser::parse(obj, updated)) {
        updated.absFile = post.absFile;
        updated.file = post.file;
        updated.ext = post.ext;
        updated.category = post.category;
        QVector<Post> posts = m_index.posts();
        bool found = false;
        for (Post &p : posts) {
            if (!p.isRemote && p.absFile == post.absFile) {
                p = updated;
                found = true;
            }
        }
        if (found) {
            m_index.setPosts(posts);
            if (BrowserTab *t = currentTab();
                t && !t->isRemote && !t->isFavourites)
                t->localPosts = posts;
            rebuildCombos();
            rebuildSidebar();
            applyFilter();
            if (m_pages->currentIndex() == 1) {
                m_viewer->setPosts(m_current);
                for (int i = 0; i < m_current.size(); ++i) {
                    if (!m_current[i].isRemote &&
                        m_current[i].absFile == post.absFile) {
                        m_viewer->showRow(i);
                        break;
                    }
                }
            }
            maybeClassify();
        }
    }
    statusBar()->showMessage(tr("Tags saved"), 4000);
}

void MainWindow::importLosnBackup() {
    const QString path = QFileDialog::getOpenFileName(
        this, tr("LoliSnatcher backup"), QDir::homePath(),
        tr("SQLite database (*.db);;All files (*)"));
    if (path.isEmpty())
        return;
    auto *dlg = new LosnImportDialog(path, &m_favDb, m_downloads, m_settings,
                                     effectiveDownloadDir(m_settings),
                                     this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->show();
    dlg->raise();
}

void MainWindow::refreshFavTabs() {
    if (!m_favDb.isOpen())
        return;
    const QVector<Post> favs = m_favDb.list();
    for (int i = 0; i < m_tabs.size(); ++i) {
        if (!m_tabs[i]->isFavourites)
            continue;
        m_tabs[i]->localPosts = favs;
        updateTabTitle(i);
    }
    // The visible tab re-filters too (activateTab re-lists again; cheap).
    if (BrowserTab *t = currentTab(); t && t->isFavourites)
        activateTab(m_curTab);
}

QString MainWindow::describeLastPlace() const {
    if (!m_settings.hasLastPlace || m_settings.lastPlace.kind.isEmpty())
        return {};
    const AppSettings::SavedTab &lp = m_settings.lastPlace;
    if (lp.kind == QStringLiteral("favs"))
        return tr("Favourites");
    if (lp.kind == QStringLiteral("browse")) {
        const QStringList shown = siteDisplayNames(lp.sites);
        const QString where = lp.tags.isEmpty()
                                  ? shown.join(QStringLiteral(" + "))
                                  : lp.tags.join(u' ');
        return where.isEmpty() ? tr("Browse") : where;
    }
    const QString name = QDir(lp.root).dirName();
    return name.isEmpty() ? lp.root : name;
}

void MainWindow::openLastPlace() {
    if (!m_settings.hasLastPlace || m_settings.lastPlace.kind.isEmpty())
        return;
    const AppSettings::SavedTab lp = m_settings.lastPlace;
    // Reuse the tab when it is still open (keeps its live state).
    if (lp.kind == QStringLiteral("favs")) {
        int favIdx = -1;
        for (int i = 0; i < m_tabs.size(); ++i) {
            if (m_tabs[i]->isFavourites) {
                favIdx = i;
                break;
            }
        }
        if (favIdx < 0) {
            auto *t = new BrowserTab();
            t->isFavourites = true;
            t->sort = QStringLiteral("added"); // DB order is newest-first
            t->title = tr("Favourites");
            m_tabs.append(t);
            m_tabBar->addTab(QString());
            updateTabTitle(m_tabs.size() - 1);
            favIdx = m_tabs.size() - 1;
        }
        BrowserTab *t = m_tabs[favIdx];
        t->activeTags = lp.tags;
        t->searchQuery = lp.query;
        t->rating = lp.rating;
        t->sort = lp.sort;
        if (m_tabBar->currentIndex() == favIdx + 1)
            activateTab(favIdx); // already there: refresh in place
        else
            m_tabBar->setCurrentIndex(favIdx + 1);
        saveTabs();
        return;
    }
    if (lp.kind == QStringLiteral("browse") && !lp.sites.isEmpty()) {
        for (int i = 0; i < m_tabs.size(); ++i) {
            BrowserTab *t = m_tabs[i];
            if (t->isRemote && !t->isFavourites && t->sites == lp.sites) {
                t->activeTags = lp.tags;
                t->searchQuery = lp.query;
                t->rating = lp.rating;
                t->sort = lp.sort;
                if (m_tabBar->currentIndex() == i + 1)
                    activateTab(i); // already there: refresh in place
                else
                    m_tabBar->setCurrentIndex(i + 1);
                return;
            }
        }
        auto *t = new BrowserTab();
        t->isRemote = true;
        t->sites = lp.sites;
        t->activeTags = lp.tags;
        t->searchQuery = lp.query;
        t->rating = lp.rating;
        t->sort = lp.sort;
        t->page = 1;
        m_tabs.append(t);
        m_tabBar->addTab(QString());
        updateTabTitle(m_tabs.size() - 1);
        m_tabBar->setCurrentIndex(m_tabs.size()); // +1: tab 0 is welcome
        saveTabs();
        return;
    }
    if (lp.kind == QStringLiteral("local") && !lp.root.isEmpty()) {
        if (!QDir(lp.root).exists()) {
            statusBar()->showMessage(
                tr("Last place is gone: %1").arg(lp.root), 6000);
            return;
        }
        for (int i = 0; i < m_tabs.size(); ++i) {
            BrowserTab *t = m_tabs[i];
            if (!t->isRemote && !t->isFavourites && t->root == lp.root) {
                t->activeTags = lp.tags;
                t->searchQuery = lp.query;
                t->rating = lp.rating;
                t->category = lp.category;
                t->sort = lp.sort;
                if (m_tabBar->currentIndex() == i + 1)
                    activateTab(i); // already there: refresh in place
                else
                    m_tabBar->setCurrentIndex(i + 1);
                return;
            }
        }
        auto *t = new BrowserTab();
        t->isRemote = false;
        t->root = lp.root;
        t->activeTags = lp.tags;
        t->searchQuery = lp.query;
        t->rating = lp.rating;
        t->category = lp.category;
        t->sort = lp.sort;
        m_tabs.append(t);
        m_tabBar->addTab(QString());
        updateTabTitle(m_tabs.size() - 1);
        m_tabBar->setCurrentIndex(m_tabs.size()); // +1: tab 0 is welcome
        saveTabs();
    }
}

void MainWindow::openFavTab() {    for (int i = 0; i < m_tabs.size(); ++i) {
        if (m_tabs[i]->isFavourites) {
            m_tabBar->setCurrentIndex(i + 1);
            return;
        }
    }
    auto *t = new BrowserTab();
    t->isFavourites = true;
    t->sort = QStringLiteral("added"); // DB order is newest-first
    t->title = tr("Favourites");
    m_tabs.append(t);
    m_tabBar->addTab(QString());
    updateTabTitle(m_tabs.size() - 1);
    m_tabBar->setCurrentIndex(m_tabs.size());
    saveTabs();
}

// ── Tag refresh ──────────────────────────────────────────────

QString MainWindow::refreshReason(const Post &post) {
    RefreshTarget t = resolveRefreshTarget(post);
    if (!t.valid()) {
        if (!post.isRemote)
            return tr("No source link (gallery-dl dumps can't refresh)");
        const QString key = RemotePosts::keyForLabel(post.siteName);
        const BooruSite *site = key.isEmpty() ? nullptr : RemoteSource::siteFor(key);
        if (site && site->baseUrl.isEmpty())
            return tr("Set the %1 instance URL first").arg(site->label);
        return tr("Nothing refreshable");
    }
    if (t.siteKey == QStringLiteral("gelbooru") && !credsFor(t.siteKey).complete())
        return tr("Needs Gelbooru API key (Preferences → Booru manager)");
    return {};
}

void MainWindow::refreshPost(const Post &post) {
    RefreshTarget target = resolveRefreshTarget(post);
    if (!target.valid()) {
        statusBar()->showMessage(refreshReason(post), 6000);
        return;
    }
    // Note: target.creds already comes from the configured booru entry.
    m_refreshPost = post;
    m_refreshTarget = target;
    statusBar()->showMessage(tr("Refreshing tags…"));
    m_refresher->refresh(target);
}

void MainWindow::onRefreshFailed(qint64 reqId, const QString &error) {
    Q_UNUSED(reqId);
    statusBar()->showMessage(tr("Refresh failed: %1").arg(error), 8000);
}

namespace {
Post remapFresh(const QJsonObject &raw, const RefreshTarget &t) {
    // Engine-based so custom instances map like their family presets.
    switch (RemotePosts::engineForSiteKey(t.siteKey)) {
    case BooruEngine::Danbooru:
        return RemotePosts::fromDanbooru(raw, t.baseUrl);
    case BooruEngine::Gelbooru:
    case BooruEngine::Safebooru:
        return RemotePosts::fromGelbooru(raw, t.baseUrl);
    case BooruEngine::Szurubooru:
        return RemotePosts::fromSzuru(raw, t.baseUrl);
    case BooruEngine::Shimmie: {
        QMap<QString, QString> attrs;
        for (auto it = raw.begin(); it != raw.end(); ++it)
            attrs.insert(it.key(), it.value().toVariant().toString());
        return RemotePosts::fromShimmie(attrs, t.baseUrl);
    }
    case BooruEngine::Moebooru:
        return RemotePosts::fromMoebooru(raw, t.baseUrl);
    }
    return RemotePosts::fromMoebooru(raw, t.baseUrl);
}

// Sidecar the scanner would have used for this local file.
QString localSidecarFor(const Post &p) {
    const QFileInfo fi(p.absFile);
    const QString flat = p.absFile + QStringLiteral(".json");
    if (QFile::exists(flat))
        return flat;
    const QString stem =
        fi.absolutePath() + u'/' + fi.completeBaseName() + QStringLiteral(".json");
    if (QFile::exists(stem))
        return stem;
    return {};
}
} // namespace

void MainWindow::onRefreshDone(qint64 reqId,
                                     const QJsonObject &raw) {
    Q_UNUSED(reqId);
    const Post fresh = remapFresh(raw, m_refreshTarget);
    const Post orig = m_refreshPost;
    const QString siteKey = m_refreshTarget.siteKey;
    int tagCount = 0;

    if (orig.isRemote) {
        // In-memory update (+ downloaded copy's sidecar, if present).
        Post merged = fresh;
        merged.category = orig.category;
        auto replaceIn = [&](QVector<Post> &vec) {
            for (Post &p : vec) {
                if (p.isRemote && p.siteName == orig.siteName && p.id == orig.id)
                    p = merged;
            }
        };
        replaceIn(m_current);
        replaceIn(m_browseMerged);
        if (BrowserTab *t = currentTab())
            replaceIn(t->browseMerged);
        tagCount = merged.tags.split(QChar::Space, Qt::SkipEmptyParts).size();
        fillModel();
        rebuildBrowseSidebar();
        updateResultLabel();
        if (m_pages->currentIndex() == 1) {
            m_viewer->setPosts(m_current);
            // Re-show the same post if still present.
            for (int i = 0; i < m_current.size(); ++i) {
                const Post &p = m_current[i];
                if (p.isRemote && p.siteName == orig.siteName && p.id == orig.id) {
                    m_viewer->showRow(i);
                    break;
                }
            }
        }
        // Downloaded copy's sidecar shares the filename convention.
        const QString stem =
            RemotePosts::downloadFileName(orig.siteName, raw).split(u'.').first();
        const QString sidecar = effectiveDownloadDir(m_settings) + u'/' + orig.siteName +
                                u'/' + stem + QStringLiteral(".json");
        if (QFile::exists(sidecar)) {
            QFile f(sidecar);
            if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
                f.write(QJsonDocument(raw).toJson(QJsonDocument::Indented)); // jq-readable
        }
    } else {
        // Local file: rewrite its sidecar, then re-parse through the normal path.
        const QString sidecar = localSidecarFor(orig);
        if (sidecar.isEmpty() || !writeRefreshedSidecar(sidecar, raw, siteKey)) {
            statusBar()->showMessage(tr("Refresh: sidecar not writable"), 6000);
            return;
        }
        QFile f(sidecar);
        Post updated;
        if (!f.open(QIODevice::ReadOnly) ||
            !MetaParser::parse(
                QJsonDocument::fromJson(f.readAll()).object(), updated)) {
            statusBar()->showMessage(tr("Refresh: re-parse failed"), 6000);
            return;
        }
        updated.file = orig.file;
        updated.absFile = orig.absFile;
        updated.ext = orig.ext;
        updated.category = orig.category;
        tagCount = updated.tags.split(QChar::Space, Qt::SkipEmptyParts).size();
        QVector<Post> posts = m_index.posts();
        for (Post &p : posts) {
            if (!p.isRemote && p.absFile == orig.absFile)
                p = updated;
        }
        m_index.setPosts(posts);
        if (BrowserTab *t = currentTab(); t && !t->isRemote && !t->isFavourites)
            t->localPosts = posts;
        rebuildCombos();
        rebuildSidebar();
        applyFilter();
        if (m_pages->currentIndex() == 1) {
            m_viewer->setPosts(m_current);
            for (int i = 0; i < m_current.size(); ++i) {
                if (!m_current[i].isRemote && m_current[i].absFile == orig.absFile) {
                    m_viewer->showRow(i);
                    break;
                }
            }
        }
    }
    statusBar()->showMessage(tr("Tags refreshed (%1 tags)").arg(tagCount),
                             6000);
    updateTabTitle(m_curTab);
}

void MainWindow::openPath(const QString &path) {
    if (path.isEmpty() || !QDir(path).exists())
        return;
    addLocalTab(path);
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
    m_welcome->setRestore(m_settings.tabsDefault == QStringLiteral("last"));
    m_welcome->setLastPlace(describeLastPlace());
    m_curTab = -1;
    m_tabBar->blockSignals(true);
    m_tabBar->setCurrentIndex(0);
    m_tabBar->blockSignals(false);
    m_stage->setCurrentIndex(0);
    setWindowTitle(QStringLiteral("booru-explorer"));
}

void MainWindow::openFolder() {
    const QString dir = QFileDialog::getExistingDirectory(
        this, tr("Open booru folder"), m_root);
    if (dir.isEmpty())
        return;
    m_settings.lastRoot = dir;
    saveSettings(m_settings);
    addLocalTab(dir);
}

void MainWindow::reload() {
    if (isBrowse()) {
        doBrowseSearch();
        return;
    }
    if (BrowserTab *t = currentTab(); t && t->isFavourites) {
        refreshFavTabs(); // re-list the DB, not the archive on disk
        return;
    }
    if (m_curTab >= 0)
        startScan(m_root);
}

void MainWindow::loadStylesheet() {
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Load custom Qt stylesheet"), QDir::homePath(),
        tr("Stylesheets (*.qss);;All files (*)"));
    if (path.isEmpty())
        return;
    if (!Theme::applyStylesheetFile(*qApp, path))
        QMessageBox::warning(this, tr("Stylesheet"),
                             tr("Could not read %1").arg(path));
}

void MainWindow::cancelScan() {
    // Stop a running archive scan so rescans and quit never pile up or hang.
    if (m_scanner)
        m_scanner->requestCancel();
    if (m_scanWatcher) {
        m_scanWatcher->cancel();
        m_scanWatcher->deleteLater();
        m_scanWatcher = nullptr;
    }
    if (m_scanner) {
        m_scanner->deleteLater();
        m_scanner = nullptr;
    }
    m_scanBar->setVisible(false);
}

void MainWindow::startScan(const QString &root) {
    m_fetcher->cancel();
    cancelScan(); // never stack giant scans (quit also relies on this)
    m_root = root;
    m_stage->setCurrentIndex(1);
    setWindowTitle(QStringLiteral("booru-explorer - %1").arg(root));
    statusBar()->showMessage(tr("Scanning %1 …").arg(root));
    auto *scanner = new Scanner;
    auto *watcher = new ScanWatcher;
    m_scanner = scanner;
    m_scanWatcher = watcher;
    connect(scanner, &Scanner::progress, this, &MainWindow::onScanProgress);
    connect(scanner, &Scanner::cacheStats, this, &MainWindow::onCacheStats);
    connect(watcher, &QFutureWatcher<QVector<Post>>::finished, this,
            [this, watcher, scanner] {
                if (watcher != m_scanWatcher) {
                    // Superseded by a newer scan (or quit): drop the result.
                    watcher->deleteLater();
                    if (scanner != m_scanner)
                        scanner->deleteLater();
                    return;
                }
                m_scanWatcher = nullptr;
                m_scanner = nullptr;
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
    statusBar()->showMessage(tr("Scanning… %1 / %2 files").arg(n).arg(total));
}

void MainWindow::onCacheStats(int hits, int misses) {
    statusBar()->showMessage(
        tr("Index cache: %1 reused, %2 parsed").arg(hits).arg(misses), 8000);
}

void MainWindow::onScanFinished() {
    const Stats s = m_index.stats();
    m_countLabel->setText(tr("%1 posts").arg(s.total));
    updateDots();
    m_scanBar->setVisible(false);
    statusBar()->showMessage(tr("Found %1 posts").arg(s.total), 5000);
    // Local archives are never modified: no history, no last-root, no
    // session writes (tabs are for browsing; the library stays untouched).
    if (BrowserTab *t = currentTab(); t && !t->isRemote && t->root == m_root)
        t->localPosts = m_index.posts();
    rebuildCombos();
    rebuildSidebar();
    applyFilter();
    maybeClassify();
}

bool MainWindow::maybeClassify(bool force) {
    const bool online = m_settings.classifyTags;
    const QSet<QString> unknown =
        (online || force) ? m_index.collectUntyped() : QSet<QString>();
    if (unknown.isEmpty() && m_settings.manualTagTypes.isEmpty())
        return false;
    const QString db = effectiveTagTypesDbPath(m_settings);
    TagTypeCache cache;
    QHash<QString, QString> known;
    QStringList missing;
    if (!unknown.isEmpty() && !force && cache.open(db)) {
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
    } else if (!unknown.isEmpty()) {
        if (force)
            statusBar()->showMessage(
                tr("Refreshing categories for %1 tags…").arg(unknown.size()));
        missing = QStringList(unknown.begin(), unknown.end());
    }
    // Manual overrides: no network, win over fetched verdicts, and keep
    // pinned tags (even General) out of the fetch queue.
    for (auto it = m_settings.manualTagTypes.begin();
         it != m_settings.manualTagTypes.end(); ++it) {
        known.insert(it.key(), it.value());
        missing.removeAll(it.key());
    }
    if (!known.isEmpty()) {
        const int moved = m_index.applyTagTypes(known);
        if (moved > 0) {
            rebuildCombos();
            rebuildSidebar();
            applyFilter();
            statusBar()->showMessage(
                tr("Categorized %1 tags from cache").arg(moved), 8000);
        }
    }
    if (!missing.isEmpty() && (online || force)) {
        missing.sort();
        statusBar()->showMessage(
            tr("Classifying %1 untyped tags…").arg(missing.size()));
        m_fetcher->fetch(missing, db);
        return true;
    }
    return false;
}

int MainWindow::applyHostBuckets(QVector<Post> &work,
                                 const QHash<QString, QString> &buckets) {
    if (buckets.isEmpty() || work.isEmpty())
        return 0;
    // Group by entry so each group's verdicts fold through its own
    // taxonomy (multibooru: a meme host and danbooru classify together).
    QHash<QString, QVector<int>> groups;
    for (int i = 0; i < work.size(); ++i) {
        const Post &p = work.at(i);
        const QString key = !p.siteKey.isEmpty()
                                ? p.siteKey
                                : RemotePosts::keyForLabel(p.siteName);
        groups[key].append(i);
    }
    int moved = 0;
    for (auto it = groups.begin(); it != groups.end(); ++it) {
        QHash<QString, QString> folded = buckets;
        const Post &rep = work.at(it.value().first());
        for (auto b = folded.begin(); b != folded.end(); ++b)
            b.value() = RemotePosts::foldedBucketFor(rep, b.value());
        QVector<Post> sub;
        sub.reserve(it.value().size());
        for (int i : it.value())
            sub.append(work.at(i));
        moved += applyTagBuckets(sub, folded).size();
        for (int j = 0; j < it.value().size(); ++j)
            work[it.value().at(j)] = sub.at(j);
    }
    return moved;
}

void MainWindow::maybeClassifyBrowse(BrowserTab *t) {    if (!t || t->isFavourites || !m_settings.classifyTags)
        return;
    // Manual overrides are explicit user intent: they apply to every post,
    // opted-out entries included. Fetched/cached verdicts do not.
    QHash<QString, QString> knownManual;
    for (auto it = m_settings.manualTagTypes.begin();
         it != m_settings.manualTagTypes.end(); ++it)
        knownManual.insert(it.key(), it.value());
    int manualMoved = 0;
    if (!knownManual.isEmpty())
        manualMoved = applyTagBuckets(t->browseMerged, knownManual).size();
    // Entries that opt out keep their tags as they arrive: collect and
    // apply verdicts for the rest only, preserving post order.
    QVector<Post> work;
    QVector<int> workIndex;
    for (int i = 0; i < t->browseMerged.size(); ++i) {
        if (!RemotePosts::skipAutoClassify(t->browseMerged.at(i))) {
            work.append(t->browseMerged.at(i));
            workIndex.append(i);
        }
    }
    QSet<QString> unknown;
    for (const Post &p : work) {
        for (const QString &tag :
             p.untyped.split(QChar::Space, Qt::SkipEmptyParts))
            unknown.insert(tag.toLower());
    }
    if (unknown.isEmpty() && manualMoved == 0)
        return;
    const QString db = effectiveTagTypesDbPath(m_settings); // one store for everything
    TagTypeCache cache;
    QHash<QString, QString> known;
    QStringList missing;
    if (!unknown.isEmpty() && cache.open(db)) {
        for (const QString &tag : unknown) {
            int ty = TagTypes::kTransientFailure;
            if (cache.get(tag, ty)) {
                if (ty != TagTypes::kUnknownTag)
                    known.insert(tag, TagTypes::bucketForType(ty));
                // negatives stay general without refetching
            } else {
                missing.append(tag);
            }
        }
        cache.close();
    } else if (!unknown.isEmpty()) {
        missing = QStringList(unknown.begin(), unknown.end());
    }
    // Manual overrides win over fetched verdicts (and need no fetch).
    for (auto it = knownManual.begin(); it != knownManual.end(); ++it) {
        known.insert(it.key(), it.value());
        missing.removeAll(it.key());
    }
    if (!known.isEmpty() || manualMoved > 0) {
        const int moved =
            manualMoved + applyHostBuckets(work, known);
        for (int j = 0; j < work.size(); ++j)
            t->browseMerged[workIndex.at(j)] = work.at(j);
        if (t == currentTab()) {
            m_browseMerged = t->browseMerged;
            applyBrowseFilter();
            rebuildBrowseSidebar();
            rebuildCombos();
            updateResultLabel();
            if (moved > 0)
                statusBar()->showMessage(
                    tr("Categorized %1 tags from cache").arg(moved), 8000);
        }
    }
    if (!missing.isEmpty()) {
        missing.sort();
        m_classifyTab = t;
        m_browseFetcher->fetch(missing, db);
        if (t == currentTab())
            statusBar()->showMessage(
                tr("Classifying %1 untyped tags…").arg(missing.size()));
    }
}

void MainWindow::onBrowseClassifyFinished(
    const QHash<QString, QString> &buckets) {
    m_scanBar->setVisible(false);
    BrowserTab *t = m_classifyTab;
    m_classifyTab = nullptr;
    m_pendingBrowseBuckets.clear(); // full map below supersedes partials
    if (!t || !m_tabs.contains(t))
        return; // tab closed mid-fetch
    const int failed = m_browseFetcher->failures();
    if (buckets.isEmpty()) {
        if (t == currentTab())
            statusBar()->showMessage(
                failed > 0
                    ? tr("Tag classification: %1 failed (network), "
                         "will retry next scan")
                          .arg(failed)
                    : tr("Tag classification done"),
                8000);
        return;
    }
    // Fetched verdicts skip opted-out entries (manual overrides were
    // already applied up front and are unaffected).
    QVector<Post> work;
    QVector<int> workIndex;
    for (int i = 0; i < t->browseMerged.size(); ++i) {
        if (!RemotePosts::skipAutoClassify(t->browseMerged.at(i))) {
            work.append(t->browseMerged.at(i));
            workIndex.append(i);
        }
    }
    int moved = applyHostBuckets(work, buckets);
    for (int j = 0; j < work.size(); ++j)
        t->browseMerged[workIndex.at(j)] = work.at(j);
    if (t != currentTab())
        return; // stashed; shows classified when activated
    m_browseMerged = t->browseMerged;
    applyBrowseFilter();
    rebuildBrowseSidebar();
    rebuildCombos();
    updateResultLabel();
    QString msg = tr("Categorized %1 tags (%2 resolved)")
                      .arg(moved)
                      .arg(buckets.size());
    if (failed > 0)
        msg += tr(" · %1 failed, will retry").arg(failed);
    statusBar()->showMessage(msg, 8000);
}

bool MainWindow::classifyUiTick(int done, int total) {
    // 1500 status-bar relayouts + bar paints is its own freeze: refresh at
    // most every 200ms (and always on completion).
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (done < total && now - m_lastClassifyUi < 200)
        return false;
    m_lastClassifyUi = now;
    return true;
}

void MainWindow::onClassifyProgress(int done, int total) {
    if (!classifyUiTick(done, total))
        return;
    m_scanBar->setVisible(true);
    m_scanBar->setRange(0, qMax(1, total));
    m_scanBar->setValue(done);
    statusBar()->showMessage(
        tr("Classifying tags… %1 / %2").arg(done).arg(total));
}

void MainWindow::onClassifyPartial(const QHash<QString, QString> &buckets) {
    if (m_manualActive || buckets.isEmpty())
        return; // manual popup run owns its own UI
    for (auto it = buckets.begin(); it != buckets.end(); ++it)
        m_pendingBuckets.insert(it.key(), it.value());
    if (!m_partialTimer->isActive())
        m_partialTimer->start();
}

void MainWindow::onBrowseClassifyPartial(
    const QHash<QString, QString> &buckets) {
    if (buckets.isEmpty())
        return;
    for (auto it = buckets.begin(); it != buckets.end(); ++it)
        m_pendingBrowseBuckets.insert(it.key(), it.value());
    if (!m_partialTimer->isActive())
        m_partialTimer->start();
}

void MainWindow::flushClassifyPartials() {
    if (!m_pendingBuckets.isEmpty()) {
        const QHash<QString, QString> batch = std::move(m_pendingBuckets);
        m_pendingBuckets.clear();
        if (m_index.applyTagTypes(batch) > 0 && !isBrowse())
            rebuildSidebar();
    }
    if (!m_pendingBrowseBuckets.isEmpty()) {
        const QHash<QString, QString> batch = std::move(m_pendingBrowseBuckets);
        m_pendingBrowseBuckets.clear();
        BrowserTab *t = m_classifyTab;
        if (!t || !m_tabs.contains(t))
            return; // tab closed mid-fetch; finished() drops it too
        QVector<Post> work;
        QVector<int> workIndex;
        for (int i = 0; i < t->browseMerged.size(); ++i) {
            if (!RemotePosts::skipAutoClassify(t->browseMerged.at(i))) {
                work.append(t->browseMerged.at(i));
                workIndex.append(i);
            }
        }
        if (!work.isEmpty()) {
            applyHostBuckets(work, batch);
            for (int j = 0; j < work.size(); ++j)
                t->browseMerged[workIndex.at(j)] = work.at(j);
            if (t == currentTab()) {
                m_browseMerged = t->browseMerged;
                rebuildBrowseSidebar();
            }
        }
    }
}

void MainWindow::onClassifyFinished(const QHash<QString, QString> &buckets) {
    if (m_manualActive)
        return; // manual popup run: onManualClassifyFinished owns it
    m_pendingBuckets.clear(); // everything below re-applies the full map
    m_scanBar->setVisible(false);
    const int failed = m_fetcher->failures();
    if (buckets.isEmpty()) {
        statusBar()->showMessage(
            failed > 0
                ? tr("Tag classification: %1 failed (network), "
                                 "will retry next scan")
                      .arg(failed)
                : tr("Tag classification done"),
            8000);
        return;
    }
    const int moved = m_index.applyTagTypes(buckets);
    rebuildCombos();
    rebuildSidebar();
    applyFilter();
    QString msg = tr("Categorized %1 tags (%2 resolved)")
                      .arg(moved)
                      .arg(buckets.size());
    if (failed > 0)
        msg += tr(" · %1 failed, will retry").arg(failed);
    statusBar()->showMessage(msg, 8000);
}

void MainWindow::rebuildCombos() {
    const Stats s = m_index.stats();
    // Browse tabs filter the merged results: counts come from them, not
    // from whatever local archive was scanned last.
    QHash<QString, int> browseRatings;
    int browseTotal = 0;
    if (isBrowse()) {
        for (const Post &p : m_browseMerged) {
            browseRatings[p.rating]++;
            browseTotal++;
        }
    }
    m_ratingBox->blockSignals(true);
    m_ratingBox->clear();
    const QList<QPair<QString, QString>> ratings = {
        {QStringLiteral("all"), tr("All")},
        {QStringLiteral("general"), tr("General")},
        {QStringLiteral("sensitive"), tr("Sensitive")},
        {QStringLiteral("questionable"), tr("Questionable")},
        {QStringLiteral("explicit"), tr("Explicit")},
    };
    for (const auto &[key, label] : ratings) {
        const int n = isBrowse() ? (key == QStringLiteral("all") ? browseTotal
                                                                 : browseRatings.value(key))
                                 : (key == QStringLiteral("all") ? s.total
                                                                 : s.ratings.value(key));
        m_ratingBox->addItem(QStringLiteral("%1 (%2)").arg(label).arg(n), key);
    }
    const int ri = m_ratingBox->findData(m_rating);
    m_ratingBox->setCurrentIndex(qMax(0, ri));
    m_ratingBox->blockSignals(false);

    m_categoryBox->blockSignals(true);
    m_categoryBox->clear();
    m_categoryBox->addItem(tr("All sources (%1)").arg(s.total), QString());
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
    // Local tabs have no host: every section is always on (undoes any
    // browse-tab hiding below).
    for (int i = 0; i < m_sideSplit->count(); ++i)
        m_sideSplit->widget(i)->setVisible(true);
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
    if (isBrowse()) {
        applyBrowseFilter();
        return;
    }
    BrowserTab *t = currentTab();
    if (t && t->isFavourites) {
        // Client-side filter over the DB list (no index involved).
        QStringList inc, exc;
        splitIncExc(m_activeTags + m_searchQuery.split(QChar::Space, Qt::SkipEmptyParts),
                    inc, exc);
        const QSet<QString> banned(m_settings.blacklist.begin(),
                                   m_settings.blacklist.end());
        QVector<Post> out;
        for (const Post &p : t->localPosts) {
            if (m_rating != QStringLiteral("all") && p.rating != m_rating)
                continue;
            if (blacklisted(p, banned))
                continue;
            if (matchesTokens(p, inc, exc))
                out.append(p);
        }
        sortPosts(out, m_sort);
        m_current = out;
        fillModel();
        rebuildBrowseSidebar();
        updateResultLabel();
        updateTabTitle(m_curTab);
        return;
    }
    QStringList tokens = m_activeTags + m_searchQuery.split(QChar::Space, Qt::SkipEmptyParts);
    for (const QString &b : m_settings.blacklist) {
        if (!b.isEmpty())
            tokens << QStringLiteral("-") + b;
    }
    const QueryResult r =
        m_index.query(tokens, m_rating, m_category, m_sort, 1, INT_MAX);
    m_current = r.posts;
    fillModel();
    updateResultLabel();
    updateTabTitle(m_curTab);
}

void MainWindow::fillModel() {
    m_model->clear();
    for (const Post &p : m_current) {
        auto *item = new QStandardItem();
        QString key;
        if (p.isRemote) {
            // The info card already shows #id: display the file name here
            // so the two lines never duplicate each other.
            const QString name = QFileInfo(p.fileUrl).fileName();
            item->setData(name.isEmpty() && p.id
                              ? QStringLiteral("#%1").arg(p.id)
                              : name,
                          Qt::DisplayRole);
            key = QStringLiteral("url|") + RemotePosts::thumbUrlFor(p);
        } else {
            item->setData(QFileInfo(p.file).fileName(), Qt::DisplayRole);
            const QFileInfo fi(p.absFile);
            key = p.absFile + u'|' + QString::number(fi.lastModified().toMSecsSinceEpoch());
        }
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
        item->setData(p.isRemote ? QString() : p.absFile, Qt::UserRole + 5);
        // Local posts carry a hash id, not a booru number: show a dash.
        item->setData(p.isRemote && p.id ? QStringLiteral("#%1").arg(p.id)
                                         : QStringLiteral("-"),
                      Qt::UserRole + 6);
        // Origin badge: the configured entry's text + color (never the
        // engine-family code). Browsed posts carry their entry key;
        // favourites and older rows resolve by label instead.
        QString badge, badgeColor;
        if (p.isRemote) {
            const BooruSite *entry = nullptr;
            if (!p.siteKey.isEmpty())
                entry = RemoteSource::siteFor(p.siteKey);
            if (!entry) {
                const QString k = RemotePosts::keyForLabel(p.siteName);
                entry = k.isEmpty() ? nullptr : RemoteSource::siteFor(k);
            }
            if (entry) {
                badge = entry->shortName.isEmpty() ? p.siteShort
                                                   : entry->shortName;
                badgeColor = entry->badgeColor;
            } else {
                badge = p.siteShort;
            }
        }
        item->setData(badge, Qt::UserRole + 7);
        item->setData(badgeColor, Qt::UserRole + 8);
        item->setData(isAiPost(p), Qt::UserRole + 9); // grid "AI" chip
        item->setEditable(false);
        item->setToolTip(p.isRemote ? p.postUrl : p.file);
        m_model->appendRow(item);
    }
    requestVisibleThumbs();
}

void MainWindow::updateResultLabel() {
    int flagged = 0;
    for (const Post &p : m_current) {
        if (p.ratingInferred && p.rating == QStringLiteral("explicit"))
            flagged++;
    }
    QString text = tr("%1 results").arg(m_current.size());
    if (flagged > 0)
        text += tr(" · %1 auto-flagged explicit").arg(flagged);
    m_resultLabel->setText(text);
    updateDots();
    m_pills->clear();
    const QColor accent(0x7c, 0x9e, 0xf8);
    auto addPill = [this](const QString &text, const QString &tip,
                          const QColor &color, auto &&onClick) {
        auto *b = new QPushButton(text, this);
        b->setFlat(true);
        b->setCursor(Qt::PointingHandCursor);
        b->setToolTip(tip);
        b->setStyleSheet(
            QStringLiteral("background:rgba(%1,%2,%3,.15);border:1px solid "
                           "rgba(%1,%2,%3,.4);border-radius:4px;padding:2px 7px;"
                           "color:#%4;")
                .arg(color.red())
                .arg(color.green())
                .arg(color.blue())
                .arg(color.rgb() & 0xffffff, 6, 16, QChar(u'0')));
        connect(b, &QPushButton::clicked, this, onClick);
        m_pills->addWidget(b);
    };
    QHash<QString, QString> browseBuckets;
    if (isBrowse()) {
        for (const Post &p : m_current) {
            const std::pair<QString, const char *> buckets[] = {
                {p.tagsArtist, "artist"}, {p.tagsCharacter, "character"},
                {p.tagsCopyright, "copyright"}, {p.tagsGeneral, "general"},
                {p.tagsMeta, "meta"},
            };
            for (const auto &[blob, bucket] : buckets) {
                for (const QString &tok :
                     blob.split(QChar::Space, Qt::SkipEmptyParts)) {
                    if (!browseBuckets.contains(tok))
                        browseBuckets.insert(tok, QString::fromLatin1(bucket));
                }
            }
        }
    }
    auto pillBucket = [&](const QString &tag) {
        if (!isBrowse())
            return m_index.bucketOf(tag);
        return browseBuckets.value(tag, QStringLiteral("general"));
    };
    for (const QString &t : m_activeTags) {
        const QString clean =
            t.startsWith(u'-') && t.size() > 1 ? t.mid(1) : t;
        addPill(QStringLiteral("%1 ×").arg(t), tr("Remove filter"),
                UiIcons::tagBucketColor(pillBucket(clean)),
                [this, t] {
                    m_activeTags.removeAll(t);
                    updateResultLabel(); // pill vanishes now, fetch lands later
                    commitSearch();
                });
    }
    if (!m_searchQuery.trimmed().isEmpty()) {
        // One card per OR-group (a group mixes buckets, so it takes the
        // accent like the other meta pills). Any plain remainder keeps
        // the old single query pill (legacy tabs predate the split).
        const QueryOr::ParsedQuery qpq = QueryOr::parse(m_searchQuery);
        int gi = 0;
        for (const QStringList &alts : qpq.groups) {
            const QString label =
                alts.size() == 1
                    ? QStringLiteral("~%1").arg(alts.first())
                    : QStringLiteral("~(%1)").arg(alts.join(u' '));
            addPill(label + QStringLiteral(" ×"), tr("Remove group"), accent,
                    [this, gi] { removeQueryGroup(gi); });
            ++gi;
        }
        if (!qpq.andTokens.isEmpty()) {
            addPill(QStringLiteral("⌕“%1” ×").arg(qpq.andTokens.join(u' ')),
                    tr("Clear search"), accent, [this] {
                        // Drop the plain remainder, keep any group cards.
                        m_searchQuery = canonicalQuery(
                            {}, QueryOr::parse(m_searchQuery).groups);
                        m_searchEdit->clear();
                        updateResultLabel();
                        commitSearch();
                    });
        }
    }
    if (m_rating != QStringLiteral("all")) {
        addPill(QStringLiteral("[%1] ×").arg(m_rating), tr("Clear rating"),
                accent, [this] {
                    m_rating = QStringLiteral("all");
                    updateResultLabel();
                    if (isBrowse())
                        commitSearch(); // drop it server-side too
                    else
                        commitFilter();
                });
    }
    if (!m_category.isEmpty()) {
        addPill(QStringLiteral("{%1} ×").arg(m_category),
                tr("Clear source"), accent, [this] {
                    m_category.clear();
                    updateResultLabel();
                    commitFilter();
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
        if (p.isRemote) {
            const QString thumb = RemotePosts::thumbUrlFor(p);
            if (thumb.isEmpty())
                continue;
            const QString key = QStringLiteral("url|") + thumb;
            if (m_thumbCache.contains(key))
                continue;
            if (isVideoExt(p.ext) && !RemotePosts::isImageUrl(thumb)) {
                // No image preview: decode a frame via the player (slow).
                if (p.fileUrl.isEmpty())
                    continue;
                m_videoThumbs->request(key, p.fileUrl, 0, 0,
                                       m_settings.thumbLongSide);
            } else {
                // Image preview (incl. video thumbnails): fast pool decode.
                m_thumbs->request(key, thumb, 0, 0, m_settings.thumbLongSide,
                                  RemotePosts::mediaRefererFor(p));
            }
            continue;
        }
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
    // Suggestions follow the tag being typed (2nd, 3rd, inside ~(groups)),
    // not just the first one.
    const QString bare = QueryOr::suggestBare(text);
    if (bare.length() < 2)
        return;
    if (isBrowse()) {
        // Remote tags: ask the tab's first real site (cached per site+prefix).
        BrowserTab *t = currentTab();
        const QStringList sites = t ? t->sites : m_settings.browseSites;
        const BooruSite *site = nullptr;
        for (const QString &key : sites) {
            if (isVirtualSource(key))
                continue;
            site = RemoteSource::siteFor(key);
            if (site)
                break;
        }
        if (!site) {
            m_suggester->cancel();
            return;
        }
        m_suggester->suggest(*site, credsFor(site->key), bare);
        return;
    }
    m_suggester->cancel();
    const auto sug = m_index.suggest(bare, 12);
    QStringList words, buckets;
    for (const auto &s : sug) {
        words << s.tag;
        buckets << s.bucket;
    }
    fillSuggestModel(words, buckets);
    if (!words.isEmpty()) {
        // Filter on the current token: without this the popup matches the
        // whole multi-tag line and stays empty past the first tag.
        m_completer->setCompletionPrefix(bare);
        m_completer->complete();
    }
}

void MainWindow::fillSuggestModel(const QStringList &tags,
                                  const QStringList &buckets) {
    m_completeModel->clear();
    for (int i = 0; i < tags.size(); ++i) {
        auto *item = new QStandardItem(tags.at(i));
        item->setData(i < buckets.size() ? buckets.at(i)
                                         : QStringLiteral("general"),
                      Qt::UserRole + 1);
        item->setEditable(false);
        m_completeModel->appendRow(item);
    }
}

void MainWindow::onRemoteSuggest(const QString &siteKey, const QString &prefix,
                                 QVector<TagSuggestion> suggestions) {
    Q_UNUSED(siteKey);
    // Stale results (user kept typing) must not overwrite the popup.
    const QString bare = QueryOr::suggestBare(m_searchEdit->text());
    if (bare != prefix || !isBrowse())
        return;
    QStringList words, buckets;
    for (const TagSuggestion &sg : suggestions) {
        words << sg.tag;
        buckets << sg.bucket;
    }
    fillSuggestModel(words, buckets);
    if (!words.isEmpty()) {
        m_completer->setCompletionPrefix(bare);
        m_completer->complete();
    }
}

void MainWindow::onSearchReturn() {
    m_suggester->cancel();
    m_completer->popup()->hide();
    // The omnibar feeds the card bar below: plain tags become one card
    // each (accumulating with existing cards), OR-groups stay a query
    // but render as one card per group (see updateResultLabel).
    const QueryOr::ParsedQuery pq =
        QueryOr::parse(m_searchEdit->text().trimmed());
    for (const QString &tok : pq.andTokens) {
        if (!tok.isEmpty() && !m_activeTags.contains(tok))
            m_activeTags.append(tok);
    }
    m_searchQuery = canonicalQuery({}, pq.groups);
    m_searchEdit->clear();
    commitSearch();
}

// Query string round-trip for the group cards: lone `~tag` and `~(...)`
// rebuild identically so removing one card never rewrites the others.
QString MainWindow::canonicalQuery(const QStringList &andTokens,
                                   const QList<QStringList> &groups) {
    QStringList parts = andTokens;
    for (const QStringList &alts : groups) {
        if (alts.size() == 1)
            parts << QStringLiteral("~") + alts.first();
        else
            parts << QStringLiteral("~(") + alts.join(u' ') + u')';
    }
    return parts.join(u' ');
}

void MainWindow::removeQueryGroup(int gi) {
    const QueryOr::ParsedQuery pq = QueryOr::parse(m_searchQuery);
    if (gi < 0 || gi >= pq.groups.size())
        return;
    QList<QStringList> groups = pq.groups;
    groups.removeAt(gi);
    m_searchQuery = canonicalQuery(pq.andTokens, groups);
    updateResultLabel(); // card vanishes now, fetch lands later
    commitSearch();
}

void MainWindow::commitSearch() {
    // Tags or query text changed: save to tab, refetch remote / refilter local.
    saveTabState();
    if (isBrowse()) {
        if (BrowserTab *t = currentTab())
            t->page = 1;
        updateTabTitle(m_curTab);
        doBrowseSearch();
    } else {
        rebuildSidebar();
        applyFilter();
    }
}

void MainWindow::commitFilter() {
    // Sort/rating changed: client-side in both modes, no refetch.
    saveTabState();
    // Sync combos in case the change came from pills, not the combos.
    m_ratingBox->blockSignals(true);
    m_ratingBox->setCurrentIndex(qMax(0, m_ratingBox->findData(m_rating)));
    m_ratingBox->blockSignals(false);
    m_categoryBox->blockSignals(true);
    m_categoryBox->setCurrentIndex(qMax(0, m_categoryBox->findData(m_category)));
    m_categoryBox->blockSignals(false);
    if (isBrowse())
        applyBrowseFilter();
    else
        applyFilter();
}

void MainWindow::saveTabState() {
    BrowserTab *t = currentTab();
    if (!t)
        return;
    t->activeTags = m_activeTags;
    t->searchQuery = m_searchQuery;
    t->rating = m_rating;
    t->category = m_category;
    t->sort = m_sort;
    if (t->isFavourites) {
        // localPosts is the DB list, refreshed on activation; never overwrite.
    } else if (!t->isRemote) {
        t->localPosts = m_index.posts();
    } else {
        t->browseMerged = m_browseMerged;
    }
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
    if (isBrowse()) {
        commitSearch(); // refetch with the rating in the query + busy bar
        return;
    }
    commitFilter();
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
            this, tr("Custom thumbnail ratio"),
            tr("Aspect as W:H (e.g. 4:3):"), QLineEdit::Normal,
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
    fillModel();
}

void MainWindow::setBoxW(int w) {
    if (!m_delegate || !m_grid)
        return;
    m_delegate->setBoxW(w);
    m_placeholder = QPixmap(m_delegate->boxW(),
                            (m_delegate->boxW() * m_aspectDen) / m_aspectNum);
    m_placeholder.fill(QColor(0x1e, 0x21, 0x30));
    m_grid->refreshLayout();
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
            last, tr("Custom %1:%2").arg(m_aspectNum).arg(m_aspectDen));
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

void MainWindow::runClassifyPopup() {
    BrowserTab *t = currentTab();
    if (!t || (!t->isRemote && !t->isFavourites && m_index.posts().isEmpty())) {
        statusBar()->showMessage(tr("Open an archive first"), 4000);
        return;
    }
    if (m_classifyPop) {
        m_classifyPop->raise();
        m_classifyPop->activateWindow();
        return;
    }
    auto *pop = new QDialog(this);
    pop->setAttribute(Qt::WA_DeleteOnClose);
    pop->setWindowTitle(tr("Updating tag categories"));
    pop->resize(400, 150);
    auto *lay = new QVBoxLayout(pop);
    auto *note = new QLabel(
        tr("Verdicts are shared across tabs and booru types: "
           "each tag is fetched once, ever."), pop);
    note->setWordWrap(true);
    lay->addWidget(note);
    auto *label = new QLabel(tr("Starting…"), pop);
    label->setObjectName(QStringLiteral("status"));
    label->setWordWrap(true);
    auto *bar = new CenterProgressBar(pop);
    bar->setObjectName(QStringLiteral("bar"));
    bar->setRange(0, 1);
    bar->setValue(0);
    auto *cancelBtn = new QPushButton(tr("Cancel"), pop);
    cancelBtn->setObjectName(QStringLiteral("cancelBtn"));
    auto *row = new QHBoxLayout();
    row->addStretch(1);
    row->addWidget(cancelBtn);
    lay->addWidget(label);
    lay->addWidget(bar);
    lay->addLayout(row);
    m_classifyPop = pop;
    connect(pop, &QObject::destroyed, this, [this] {
        m_classifyPop = nullptr;
        m_manualActive = false;
        m_manualTab = nullptr;
    });
    connect(cancelBtn, &QPushButton::clicked, this, [this, pop, cancelBtn] {
        // Finished runs repurpose the button as Close (see below).
        if (cancelBtn->text() == tr("Close")) {
            pop->close();
            return;
        }
        m_fetcher->cancel();
        m_manualActive = false;
        m_manualTab = nullptr;
        pop->close();
    });

    connect(m_fetcher, &TagTypeFetcher::progress, pop,
            [this, bar, label](int done, int total) {
                if (!classifyUiTick(done, total))
                    return;
                bar->setRange(0, qMax(1, total));
                bar->setValue(done);
                label->setText(tr("Classifying tags… %1 / %2").arg(done).arg(total));
            });
    connect(m_fetcher, &TagTypeFetcher::backoff, pop,
            [label] {
                label->setText(tr("Tag API rate-limited - pausing briefly…"));
            });
    connect(m_fetcher, &TagTypeFetcher::finished, pop,
            [this, pop, label, bar, cancelBtn](const QHash<QString, QString> &) {
                const int failed = m_fetcher->failures();
                bar->setRange(0, 1);
                bar->setValue(1);
                label->setText(failed > 0
                    ? tr("Done with %1 failures - retry on the next update.")
                          .arg(failed)
                    : tr("All categories up to date."));
                cancelBtn->setText(tr("Close"));
            });
    pop->show();
    startManualClassify();
}

void MainWindow::startManualClassify() {
    BrowserTab *t = currentTab();
    if (!t)
        return;
    m_fetcher->cancel();
    m_browseFetcher->cancel();
    // Target posts: local archive (mirrored index), favourites, or the
    // current browse merge. Entries that opt out contribute no tags.
    QSet<QString> unknown;
    auto collectUntyped = [&](const QVector<Post> &posts) {
        for (const Post &p : posts) {
            if ((t->isRemote || t->isFavourites) &&
                RemotePosts::skipAutoClassify(p))
                continue;
            for (const QString &tag :
                 p.untyped.split(QChar::Space, Qt::SkipEmptyParts))
                unknown.insert(tag.toLower());
        }
    };
    if (!t->isRemote && !t->isFavourites) {
        collectUntyped(m_index.posts());
    } else {
        collectUntyped(t->isFavourites ? t->localPosts : t->browseMerged);
    }
    // Manual overrides apply immediately, without any fetch - to every
    // post, opted-out entries included (explicit user intent).
    QHash<QString, QString> manual;
    for (auto it = m_settings.manualTagTypes.begin();
         it != m_settings.manualTagTypes.end(); ++it)
        manual.insert(it.key(), it.value());
    QStringList missing;
    for (const QString &tag : unknown) {
        if (!manual.contains(tag))
            missing.append(tag);
    }
    m_manualTab = t;
    if (!manual.isEmpty())
        applyManualBuckets(t, manual, false);
    if (!m_tabs.contains(t)) {
        m_manualTab = nullptr; // tab closed during apply; nothing to fetch
        return;
    }
    m_manualDb = effectiveTagTypesDbPath(m_settings); // global verdict store
    // Skip tags already resolved in the chosen database.
    QHash<QString, QString> autoKnown;
    TagTypeCache cache;
    if (cache.open(m_manualDb)) {
        QStringList fresh;
        for (const QString &tag : missing) {
            int ty = TagTypes::kTransientFailure;
            if (cache.get(tag, ty)) {
                if (ty != TagTypes::kUnknownTag)
                    autoKnown.insert(tag, TagTypes::bucketForType(ty));
            } else {
                fresh.append(tag);
            }
        }
        cache.close();
        missing.swap(fresh);
        if (!autoKnown.isEmpty())
            applyManualBuckets(t, autoKnown, true);
    }
    if (missing.isEmpty()) {
        statusBar()->showMessage(tr("All categories up to date."), 6000);
        if (m_classifyPop) {
            // Nothing to fetch: show the done state instead of a dead 0%.
            if (auto *st = m_classifyPop->findChild<QLabel *>(QStringLiteral("status")))
                st->setText(tr("All categories up to date."));
            if (auto *b = m_classifyPop->findChild<QProgressBar *>(QStringLiteral("bar"))) {
                b->setRange(0, 1);
                b->setValue(1);
            }
            if (auto *c = m_classifyPop->findChild<QPushButton *>(QStringLiteral("cancelBtn")))
                c->setText(tr("Close"));
        }
        m_manualTab = nullptr;
        return;
    }
    missing.sort();
    m_manualActive = true;
    statusBar()->showMessage(
        tr("Classifying %1 untyped tags…").arg(missing.size()));
    m_fetcher->fetch(missing, m_manualDb);
}

void MainWindow::onManualFetchDone(const QHash<QString, QString> &buckets) {
    if (!m_manualActive)
        return; // automatic runs use onClassifyFinished instead
    BrowserTab *t = m_manualTab;
    m_manualActive = false;
    m_manualTab = nullptr;
    if (buckets.isEmpty() || !t || !m_tabs.contains(t))
        return; // tab closed mid-fetch
    applyManualBuckets(t, buckets, true);
}

void MainWindow::persistFavBuckets(const QVector<Post> &posts) {
    if (!m_favDb.isOpen() || posts.isEmpty())
        return;
    QHash<QPair<QString, qint64>, FavDb::TagSplit> splits;
    for (const Post &p : posts) {
        const auto [site, pid] = favIdentity(p);
        FavDb::TagSplit s;
        s.artist = p.tagsArtist;
        s.character = p.tagsCharacter;
        s.copyright = p.tagsCopyright;
        s.general = p.tagsGeneral;
        s.meta = p.tagsMeta;
        s.untyped = p.untyped;
        splits.insert({site, pid}, s);
    }
    m_favDb.setTagSplits(splits);
}

void MainWindow::applyManualBuckets(BrowserTab *t,
                                    const QHash<QString, QString> &buckets,
                                    bool autoVerdicts) {
    if (!t || buckets.isEmpty())
        return;
    const bool current = (t == currentTab());
    const bool gridShowing = m_pages->currentIndex() == 0;
    int moved = 0;
    if (!t->isRemote && !t->isFavourites) {
        moved = m_index.applyTagTypes(buckets);
        t->localPosts = m_index.posts();
        if (current) {
            rebuildCombos();
            rebuildSidebar();
            applyFilter();
            updateTabTitle(m_curTab);
        }
    } else if (t->isFavourites) {
        // Fetched verdicts skip opted-out entries; manual overrides apply
        // to all. Splits persist to the DB (the tab re-lists from it).
        QVector<Post> work = t->localPosts;
        QVector<int> workIndex;
        if (autoVerdicts) {
            work.clear();
            for (int i = 0; i < t->localPosts.size(); ++i) {
                if (!RemotePosts::skipAutoClassify(t->localPosts.at(i))) {
                    work.append(t->localPosts.at(i));
                    workIndex.append(i);
                }
            }
        }
        QVector<Post> &target = autoVerdicts ? work : t->localPosts;
        moved = applyTagBuckets(target, buckets).size();
        if (autoVerdicts) {
            for (int j = 0; j < work.size(); ++j)
                t->localPosts[workIndex.at(j)] = work.at(j);
        }
        if (moved > 0)
            persistFavBuckets(t->localPosts);
        if (current && gridShowing) {
            // In-memory refresh only: a DB re-list here would drop the
            // just-applied buckets (they are stored now for next time).
            setWindowTitle(QStringLiteral("booru-explorer - ♥ Favourites"));
            commitFilter();
        }
    } else {
        QVector<Post> work = t->browseMerged;
        QVector<int> workIndex;
        if (autoVerdicts) {
            work.clear();
            for (int i = 0; i < t->browseMerged.size(); ++i) {
                if (!RemotePosts::skipAutoClassify(t->browseMerged.at(i))) {
                    work.append(t->browseMerged.at(i));
                    workIndex.append(i);
                }
            }
        }
        QVector<Post> &target = autoVerdicts ? work : t->browseMerged;
        moved = applyTagBuckets(target, buckets).size();
        if (autoVerdicts) {
            for (int j = 0; j < work.size(); ++j)
                t->browseMerged[workIndex.at(j)] = work.at(j);
        }
        if (current) {
            m_browseMerged = t->browseMerged;
            applyBrowseFilter();
            rebuildBrowseSidebar();
            rebuildCombos();
            updateResultLabel();
        }
    }
    if (current && gridShowing && moved > 0)
        statusBar()->showMessage(
            tr("Categorized %1 tags (%2 resolved)").arg(moved).arg(buckets.size()),
            8000);
}

void MainWindow::showAbout() {
    QMessageBox::about(
        this, tr("About booru-explorer"),
        QStringLiteral("<h3>booru-explorer %1</h3>"
                       "<p>%2</p>"
                       "<p><a href=\"%3\">%3</a><br>"
                       "Copyright (C) 2026 Lev<br>"
                       "GPLv3 or later</p>"
                       "<p>Built with Qt %4</p>")
            .arg(appVersion(), appTagline(), appRepoUrl(),
                 QString::fromLatin1(qVersion())));
}

void MainWindow::openPreferences() {
    PrefsDialog dlg(m_settings, m_favicons, this);
    connect(&dlg, &PrefsDialog::classifyNowRequested, this,
            &MainWindow::runClassifyPopup);
    connect(&dlg, &PrefsDialog::faviconsRefreshRequested, this,
            &MainWindow::refreshFavicons);
    connect(&dlg, &PrefsDialog::applyRequested, this, [this, &dlg] {
        const QString before = m_settings.language;
        applyPreferences(dlg.settings());
        offerRestartForLanguage(before);
    });
    if (dlg.exec() != QDialog::Accepted)
        return;
    {
        const QString before = m_settings.language;
        applyPreferences(dlg.settings());
        offerRestartForLanguage(before);
    }
}

void MainWindow::offerRestartForLanguage(const QString &previous) {
    // Translators install once at startup: a new locale needs a restart.
    // Session tabs persist, so this round-trips back to the same place.
    if (m_settings.language == previous)
        return;
    if (QMessageBox::question(
            this, tr("Restart needed"),
            tr("The language applies after a restart. Restart now?")) !=
        QMessageBox::Yes)
        return;
    flushSavedTabs();
    QProcess::startDetached(QCoreApplication::applicationFilePath(),
                            QCoreApplication::arguments().mid(1));
    QCoreApplication::quit();
}

void MainWindow::applyPreferences(const AppSettings &app) {
    const QString oldDataDir = effectiveDataDir(m_settings);
    const AppSettings old = m_settings;
    m_settings = app;
    saveSettings(m_settings);
    // Data folder moved: reopen the favourites DB there. The cache
    // tracking DB stays in the platform cache dir (it follows the cache,
    // not the data). Databases start fresh there — existing files stay
    // where they were unless moved over.
    if (effectiveDataDir(m_settings) != oldDataDir) {
        QDir().mkpath(effectiveDataDir(m_settings));
        m_favDb.open(effectiveDataDir(m_settings));
        refreshFavTabs();
        statusBar()->showMessage(
            tr("Data folder moved. Existing databases stay where they were "
               "unless you move them over."), 8000);
    }
    CacheJanitor::configure(
        appCacheDir(), qint64(m_settings.cacheLimitMB) * 1024 * 1024);
    setActiveExplicitTags(
        QSet<QString>(m_settings.explicitTags.begin(), m_settings.explicitTags.end()));
    RemoteSource::setConfiguredBoorus(m_settings.boorus);
    RemotePosts::setDisabledBuckets(m_settings.disabledBuckets);
    m_favicons->setCustomDir(effectiveDataDir(m_settings) +
                             QStringLiteral("/favicons"));
    WebApiFetcher::setProfilePath(effectiveDataDir(m_settings) +
                                  QStringLiteral("/webprofile"));
    prefetchFavicons();
    rebuildSitesMenu();
    // Custom favicons apply immediately (same loop as fetch arrivals).
    for (int i = 0; i < m_tabs.size(); ++i) {
        if (m_tabs[i]->isRemote)
            updateTabTitle(i);
    }
    applyBooruCreds();
    applyThemeFromSettings();
    setAspect(m_settings.aspectNum, m_settings.aspectDen);
    m_grid->setScrollRows(m_settings.gridScrollRows);
    const bool diskCachePrefs = m_settings.thumbCache;
    const QString thumbDirPrefs = appCacheDir() + u'/' + QStringLiteral("thumbs");
    m_thumbs->setCacheDir(diskCachePrefs ? thumbDirPrefs : QString());
    m_videoThumbs->setCacheDir(diskCachePrefs ? thumbDirPrefs : QString());
    m_viewer->setPlaybackDefaults(m_settings.videoMuted, m_settings.videoLoop,
                                  m_settings.volume);
    m_viewer->setMaxSide(m_settings.viewerMaxSide);
    m_welcome->setMascot(m_settings.mascotPath);
    m_imgCache->setEnabled(m_settings.fullCache);
    if (m_stage->currentIndex() == 1 && m_curTab >= 0) {
        if (isBrowse()) {
            // Re-search only when the query pipeline changed (creds, tags,
            // limit, filters): appearance/scroll edits must not refetch.
            bool sitesEqual = old.boorus.size() == m_settings.boorus.size();
            for (int i = 0; sitesEqual && i < old.boorus.size(); ++i)
                sitesEqual = booruSiteEqual(old.boorus.at(i),
                                            m_settings.boorus.at(i));
            if (!sitesEqual ||
                old.browseLimit != m_settings.browseLimit ||
                old.blacklist != m_settings.blacklist ||
                old.explicitTags != m_settings.explicitTags)
                doBrowseSearch(); // picks up credential changes
        } else if (old.explicitTags != m_settings.explicitTags ||
                   old.blacklist != m_settings.blacklist) {
            startScan(m_root); // ratings/filters changed -> rescan to apply
        }
        // else: appearance/download/speed edits leave the grid alone
    } else {
        applyFilter();
    }
}

void MainWindow::closeEvent(QCloseEvent *e) {
    cancelScan();
    m_downloads->cancel();
    m_fetcher->cancel();
    m_browseFetcher->cancel();
    m_suggester->cancel();
    m_settings.geometry = saveGeometry();
    if (m_sideSplit)
        m_settings.sideSplitter = m_sideSplit->saveState();
    flushSavedTabs(); // includes saveSettings
    QMainWindow::closeEvent(e);
}

bool MainWindow::eventFilter(QObject *o, QEvent *e) {
    if (o == statusBar() && e->type() == QEvent::MouseButtonPress &&
        m_statusProbeValid) {
        RemotePosts::showProbeDialog(this, m_statusProbeTitle, m_statusProbeText);
        return true;
    }
    return QMainWindow::eventFilter(o, e);
}

void MainWindow::clearStatusProbe() {
    m_statusProbeValid = false;
    m_statusProbeTitle.clear();
    m_statusProbeText.clear();
    statusBar()->setCursor(Qt::ArrowCursor);
    statusBar()->setToolTip(QString());
}

void MainWindow::setStatusProbe(const QString &title, const QString &text) {
    m_statusProbeTitle = title;
    m_statusProbeText = text;
    m_statusProbeValid = true;
    statusBar()->setCursor(Qt::PointingHandCursor);
    statusBar()->setToolTip(tr("Click to see the full server response"));
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
    commitSearch();
}

void MainWindow::onGridActivated(const QModelIndex &index) {
    if (!index.isValid())
        return;
    if (m_selectMode)
        return; // clicks select (double-click still opens)
    showViewer(index.row());
}

void MainWindow::downloadSelected() {
    const BrowserTab *t = currentTab();
    if (!isBrowse() && !(t && t->isFavourites))
        return;
    QVector<Post> posts;
    for (const QModelIndex &i : m_grid->selectionModel()->selectedRows()) {
        if (i.row() >= 0 && i.row() < m_current.size())
            posts.append(m_current.at(i.row()));
    }
    if (posts.isEmpty()) {
        statusBar()->showMessage(tr("Select posts first"), 3000);
        return;
    }
    downloadPosts(posts);
}

void MainWindow::updateDownloadButton() {
    const int n = m_grid && m_grid->selectionModel()
                      ? m_grid->selectionModel()->selectedRows().size()
                      : 0;
    m_downloadSelBtn->setText(tr("Download (%1)").arg(n));
    const BrowserTab *t = currentTab();
    const bool downloadable = isBrowse() || (t && t->isFavourites);
    // Always visible where downloads apply (disabled when empty): hiding
    // it on Select/selection changes shuffles the whole omnibar.
    m_downloadSelBtn->setVisible(downloadable);
    m_downloadSelBtn->setEnabled(n > 0);
}

void MainWindow::showViewer(int row) {
    const QModelIndex idx = m_model->index(row, 0);
    m_viewer->showThumbPreview(
        qvariant_cast<QPixmap>(m_model->data(idx, Qt::DecorationRole)));
    m_viewer->setPosts(m_current);
    m_viewer->showRow(row);
    if (row >= 0 && row < m_current.size()) {
        const Post &p = m_current[row];
        const auto [site, pid] = favIdentity(p);
        m_viewer->setFaved(m_favDb.isOpen() && m_favDb.isFavourite(site, pid));
        const QString reason = refreshReason(p);
        m_viewer->setRefreshable(reason.isEmpty(), reason);
    }
    m_pages->setCurrentIndex(1);
}

void MainWindow::backToGrid() {
    m_pages->setCurrentIndex(0);
    m_grid->setFocus();
    requestVisibleThumbs();
}
