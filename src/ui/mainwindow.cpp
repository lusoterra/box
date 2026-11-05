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

// Must mirror FavDb::pidFor/siteFor: remote keyed by site+id (id-less
// imports by site+URL hash), local by file hash. A downloaded favourite
// keeps its site label (rowToPost preserves it), so identity follows
// siteName whenever present - otherwise unfavouriting or deleting a
// downloaded post would miss its own database row.
std::pair<QString, qint64> favIdentity(const Post &p) {
    if (!p.siteName.isEmpty() || p.isRemote)
        return {p.siteName, FavDb::pidFor(p)};
    return {QStringLiteral("local"), FavDb::pidFor(p)};
}

// Stable per-tab viewer identity: remote rows by site#id, local rows by
// file path. Survives refills/reorders; falls back to grid when gone.
QString viewerKeyFor(const Post &post) {
    if (post.isRemote) {
        const auto [site, pid] = favIdentity(post);
        if (!site.isEmpty() && pid != 0)
            return site + u'#' + QString::number(pid);
        if (!post.fileUrl.isEmpty())
            return QStringLiteral("url|") + post.fileUrl;
        return {};
    }
    if (!post.absFile.isEmpty())
        return QStringLiteral("file|") +
               QFileInfo(post.absFile).canonicalFilePath();
    return {};
}

// Identity for list dedupe: remote rows with a content hash collapse
// across entries (Tor onion vs I2P mirror twins of one file); otherwise
// remote rows key by site+id (id-less by site+ file URL so distinct
// login-walled rows never collapse); downloaded rows by their resolved
// entry label+id so remote/local twins of one post merge into a single
// card; plain files by path. Empty = unknown, never deduped. This is
// what makes stepping past a gif never show "the same post twice" and
// keeps deleted rows from shadowing.
QString dedupeKeyFor(const Post &post) {
    if (post.isRemote) {
        if (const QString twin = md5DedupeKey(post); !twin.isEmpty())
            return twin;
        if (post.id != 0)
            return post.siteName + u'#' + QString::number(post.id);
        if (!post.fileUrl.isEmpty())
            return post.siteName + u'#' + post.fileUrl;
        return {};
    }
    if (post.id > 0) {
        if (const BooruSite *e = RemotePosts::entryForPost(post))
            return e->label + u'#' + QString::number(post.id);
    }
    if (!post.absFile.isEmpty())
        return QStringLiteral("file#") + post.absFile;
    return {};
}

// Veto key for an explicit unfavourite: a download finishing later must
// not re-favourite the row (the reported "it added itself back").
inline QString favVetoKey(const QString &site, qint64 pid) {
    return site + u'#' + QString::number(pid);
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
    // Single-letter shorthands (rating:g/s/q/e) match the stored words.
    if (want == QStringLiteral("g"))
        want = QStringLiteral("general");
    else if (want == QStringLiteral("s"))
        want = QStringLiteral("sensitive");
    else if (want == QStringLiteral("q"))
        want = QStringLiteral("questionable");
    else if (want == QStringLiteral("e"))
        want = QStringLiteral("explicit");
    return postRating.compare(want, Qt::CaseInsensitive) == 0;
}

bool matchesTokens(const Post &p, const QStringList &inc, const QStringList &exc) {
    const QStringList pt = p.tags.split(QChar::Space, Qt::SkipEmptyParts);
    // filetype: tokens match the extension, not tags (comma = OR within
    // one token, AND across tokens, like rating: above).
    QList<QSet<QString>> ftInc, ftExc;
    for (const QString &x : inc) {
        if (isFiletypeToken(x))
            ftInc.append(filetypeExtSet(x));
    }
    for (const QString &x : exc) {
        if (isFiletypeToken(x))
            ftExc.append(filetypeExtSet(x));
    }
    if (!filetypeExtOk(p.ext, ftInc, ftExc))
        return false;
    for (const QString &x : inc) {
        if (isRatingToken(x)) {
            if (!ratingMatches(p.rating, x))
                return false;
            continue;
        }
        if (isFiletypeToken(x))
            continue; // handled above
        if (!pt.contains(x))
            return false;
    }
    for (const QString &x : exc) {
        if (isRatingToken(x)) {
            if (ratingMatches(p.rating, x))
                return false;
            continue;
        }
        if (isFiletypeToken(x))
            continue; // handled above
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

// ── GalleryDelegate ──────────────────────────────────────────────

GalleryDelegate::GalleryDelegate(QObject *parent) : QStyledItemDelegate(parent) {}

void GalleryDelegate::setAspect(int num, int den) {
    m_num = qMax(1, num);
    m_den = qMax(1, den);
}

QSize GalleryDelegate::sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const {
    const int boxH = qMax(60, (m_boxW * m_den) / m_num);
    // Staggered rows reserve the half-card offset in every cell so odd
    // columns can paint lower without clipping or overlapping the row.
    const int extra = m_staggered ? (boxH + 62) / 2 : 0;
    return {m_boxW + 12, boxH + 62 + extra};
}

void GalleryDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option,
                            const QModelIndex &index) const {
    painter->save();
    const int boxH = qMax(60, (m_boxW * m_den) / m_num);
    QRect r = option.rect.adjusted(4, 4, -4, -4);
    if (m_staggered) {
        const int pitch = m_boxW + 12 + 8; // delegate width + view spacing
        if ((option.rect.x() / qMax(1, pitch)) % 2 != 0)
            r.translate(0, (boxH + 62) / 2);
        // Keep the card itself at its normal height: only the cell grew
        // to make room for the offset.
        r.setHeight(boxH + 54);
    }

    const bool sel = option.state & QStyle::State_Selected;
    const QColor accent = option.palette.color(QPalette::Highlight);
    const QColor cardBg = option.palette.color(QPalette::Base);
    const QColor cardAlt = option.palette.color(QPalette::AlternateBase);
    const QColor cardBorder = option.palette.color(QPalette::Mid);
    const QColor cardText = option.palette.color(QPalette::Text);
    const QColor cardDim = option.palette.color(QPalette::PlaceholderText);
    QColor selBg = accent;
    selBg.setAlpha(55);
    painter->setRenderHint(QPainter::Antialiasing);
    painter->setPen(sel ? accent : cardBorder);
    painter->setBrush(sel ? selBg : cardBg);
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
        painter->fillRect(imgR, cardAlt);
    }
    painter->setClipping(false);

    // Video / GIF badge (top-right over image).
    const QString kind = index.data(Qt::UserRole + 2).toString();
    if (!kind.isEmpty()) {
        const QRect kb(imgR.right() - 34, imgR.top() + 4, 30, 18);
        painter->setPen(Qt::NoPen);
        painter->setBrush(QColor(0, 0, 0, 180));
        painter->drawRoundedRect(kb, 4, 4);
        painter->setPen(accent);
        painter->drawText(kb, Qt::AlignCenter, kind);
    }

    // Status boxes, top-left over the image, left to right: purple
    // robot = AI-generated, red heart = favourited, green down
    // arrow = downloaded. Same marks as the viewer title badges.
    int markX = imgR.left() + 4;
    auto markBox = [&](const QColor &bg, const QString &glyph) {
        const QRect mb(markX, imgR.top() + 4, 30, 18);
        painter->setPen(Qt::NoPen);
        painter->setBrush(bg);
        painter->drawRoundedRect(mb, 4, 4);
        painter->setPen(Qt::white);
        painter->drawText(mb, Qt::AlignCenter, glyph);
        markX += 34;
    };
    // AI badge (top-left over image, mirrors the kind badge):
    // a robot glyph, not text.
    if (index.data(Qt::UserRole + 9).toBool()) {
        const QRect ab(imgR.left() + 4, imgR.top() + 4, 30, 18);
        painter->setPen(Qt::NoPen);
        painter->setBrush(QColor(0x7c, 0x4d, 0xff, 200));
        painter->drawRoundedRect(ab, 4, 4);
        const int cx = ab.center().x();
        const QColor paper(Qt::white);
        const QColor glass(0x7c, 0x4d, 0xff);
        painter->setBrush(paper);
        // Antenna stem + tip.
        painter->setPen(QPen(paper, 1.5));
        painter->drawLine(cx, ab.top() + 2, cx, ab.top() + 5);
        painter->setPen(Qt::NoPen);
        painter->drawEllipse(QPoint(cx, ab.top() + 3), 2, 2);
        // Head.
        painter->drawRoundedRect(cx - 7, ab.top() + 5, 14, 9, 2, 2);
        // Eyes punch out in badge purple.
        painter->setBrush(glass);
        painter->drawEllipse(QPoint(cx - 3, ab.top() + 9), 1, 1);
        painter->drawEllipse(QPoint(cx + 3, ab.top() + 9), 1, 1);
        markX += 34;
    }
    if (index.data(Qt::UserRole + 12).toBool())
        markBox(QColor(UiIcons::favRed().red(), UiIcons::favRed().green(),
                      UiIcons::favRed().blue(), 210),
                QStringLiteral("\u2665"));
    if (index.data(Qt::UserRole + 13).toBool())
        markBox(QColor(0x2e, 0x8b, 0x6e, 210), QStringLiteral("\u2193"));
    // Comments: steel-blue bubble with three white dots + tail (hand-drawn:
    // emoji speech balloons are font-dependent tofu on minimal installs).
    if (index.data(Qt::UserRole + 14).toBool()) {
        const QRect cb(markX, imgR.top() + 4, 30, 18);
        painter->setPen(Qt::NoPen);
        painter->setBrush(QColor(0x4a, 0x90, 0xd9, 210));
        painter->drawRoundedRect(cb, 4, 4);
        QPolygon tail;
        tail << QPoint(cb.left() + 7, cb.bottom() - 1)
             << QPoint(cb.left() + 11, cb.bottom() - 1)
             << QPoint(cb.left() + 8, cb.bottom() + 3);
        painter->drawPolygon(tail);
        painter->setBrush(Qt::white);
        for (int i = 0; i < 3; ++i)
            painter->drawEllipse(QPoint(cb.center().x() - 6 + i * 6,
                                       cb.center().y() - 1),
                                 1, 1);
        markX += 34;
    }
    // Annotations: amber sticky note with two dark lines.
    if (index.data(Qt::UserRole + 15).toBool()) {
        const QRect nb(markX, imgR.top() + 4, 30, 18);
        painter->setPen(Qt::NoPen);
        painter->setBrush(QColor(0xb8, 0x8b, 0x2e, 210));
        painter->drawRoundedRect(nb, 4, 4);
        painter->setBrush(QColor(0x2a, 0x20, 0x10));
        painter->drawRect(nb.left() + 7, nb.top() + 5, 16, 2);
        painter->drawRect(nb.left() + 7, nb.top() + 10, 11, 2);
        markX += 34;
    }

    // Origin badge for remote posts (bottom-left over image):
    // site favicon + short text.
    const QString origin = index.data(Qt::UserRole + 7).toString();
    if (!origin.isEmpty()) {
        const QPixmap fav =
            qvariant_cast<QPixmap>(index.data(Qt::UserRole + 10));
        const int iconW = fav.isNull() ? 0 : 20; // 16px art + padding
        const int ow =
            painter->fontMetrics().horizontalAdvance(origin) + 12 + iconW;
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
        int textLeft = 0;
        if (!fav.isNull()) {
            painter->drawPixmap(ob.left() + 4, ob.center().y() - 8, 16, 16,
                                fav);
            textLeft = iconW;
        }
        painter->setPen(Qt::white);
        painter->drawText(ob.adjusted(textLeft, 0, 0, 0), Qt::AlignCenter,
                          origin);
    }

    // Unified info card: [G] +12 #11984812
    const QRect cardR(r.left() + 5, imgR.bottom() + 4, r.width() - 10, 24);
    painter->setPen(cardBorder);
    painter->setBrush(cardAlt);
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
                                              : cardText);
    const QRect scoreR(chip.right() + 5, cardR.top(), 70, cardR.height());
    painter->drawText(scoreR, Qt::AlignVCenter | Qt::AlignLeft, score);
    painter->setPen(cardDim);
    const QRect idR(scoreR.right() + 2, cardR.top(), cardR.right() - scoreR.right() - 6,
                    cardR.height());
    painter->drawText(idR, Qt::AlignVCenter | Qt::AlignRight,
                      index.data(Qt::UserRole + 6).toString());

    // Filename line.
    painter->setPen(sel ? cardText : cardDim);
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
        const QString bucket = index.data(Qt::UserRole + 1).toString();
        const QColor meta =
            UiIcons::filterTagColor(index.data(Qt::DisplayRole).toString());
        const QColor color =
            meta.isValid() ? meta : UiIcons::tagBucketColor(bucket);
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

void GridView::resizeEvent(QResizeEvent *e) {
    QListView::resizeEvent(e);
    emit viewportResized();
}

void GridView::startDrag(Qt::DropActions) {
    const QModelIndexList sel = selectedIndexes();
    QList<QUrl> urls;
    for (const QModelIndex &i : sel) {
        // Remote posts: the configured media/post link. Downloaded/local
        // rows: the file itself (Discord uploads it like any file).
        const QString link = model()->data(i, Qt::UserRole + 20).toString();
        if (!link.isEmpty())
            urls << QUrl(link);
    }
    if (urls.isEmpty())
        return;
    auto *mime = new QMimeData();
    mime->setUrls(urls);
    QStringList plain;
    for (const QUrl &u : urls)
        plain << u.toString();
    mime->setText(plain.join(u'\n')); // paste targets that ignore urls
    auto *drag = new QDrag(this);
    drag->setMimeData(mime);
    const QPixmap pm =
        qvariant_cast<QPixmap>(model()->data(sel.first(), Qt::DecorationRole));
    if (!pm.isNull())
        drag->setPixmap(pm.scaled(128, 128, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    drag->exec(Qt::CopyAction);
}

// ── MainWindow ───────────────────────────────────────────────────

namespace {
Post remapFresh(const QJsonObject &raw, const RefreshTarget &t);
}

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
    MediaCache::configure(m_settings.cacheMode, m_settings.boorus);
    MediaCache::setSfwBlacklist(m_settings.blacklist);
    setActiveExplicitTags(
        QSet<QString>(m_settings.explicitTags.begin(), m_settings.explicitTags.end()));
    // Before the first QWebEngine* object (webDownloads below): route the
    // embedded browser through the proxy, else .onion hosts fail there
    // with ERR_NAME_NOT_RESOLVED (QNetworkProxy never reaches Chromium).
    RemotePosts::applyWebEngineProxy(m_settings.globalProxy, m_settings.boorus);
    m_favicons = new FaviconCache(appCacheDir() + QStringLiteral("/favicons"), this);
    m_favicons->setCustomDir(effectiveDataDir(m_settings) +
                             QStringLiteral("/favicons"));
    WebApiFetcher::setProfilePath(effectiveDataDir(m_settings) +
                                  QStringLiteral("/webprofile"));
    // Warm up Chromium once at idle: first WebEngine use initializes on
    // the GUI thread and can blink every window. A hidden probe now
    // means capture dialogs later open without the stall.
    QTimer::singleShot(20000, this, [this] {
        CookieGrabDialog::prime(effectiveDataDir(m_settings) +
                                QStringLiteral("/webprofile"));
    });
    RemoteSource::setWebFetcher([](const QUrl &url, int timeoutMs) {
        const WebApiFetcher::Result r =
            WebApiFetcher::fetchJsonBlocking(url, timeoutMs);
        return RemoteSource::WebFetchResult{r.http, r.body, r.error};
    });
    auto *webDownloads = new WebEngineDownloadBackend(this);
    m_downloads->setWebBackend(webDownloads);
    m_downloads->setImageCache(m_imgCache); // full-res copy shortcut
    m_downloads->setUgoiraFormat(m_settings.ugoiraFormat);
    m_downloads->setUgoiraFrameDelayMs(m_settings.ugoiraFrameDelayMs);
    m_thumbs->setWebBackend(webDownloads);
    connect(m_favicons, &FaviconCache::ready, this, &MainWindow::onFaviconReady);
    RemoteSource::setConfiguredBoorus(m_settings.boorus);
    RemotePosts::setDisabledBuckets(m_settings.disabledBuckets);
    RemotePosts::setGlobalProxy(m_settings.globalProxy);
    prefetchFavicons();
    // Remembered folder wins when no explicit root was given on the CLI.
    m_root = root;
    if (!rootExplicit && !m_settings.lastRoot.isEmpty() &&
        QDir(m_settings.lastRoot).exists())
        m_root = m_settings.lastRoot;
    setWindowTitle(QStringLiteral("Box"));
    resize(1280, 800);
    if (!m_settings.geometry.isEmpty())
        restoreGeometry(m_settings.geometry);

    applyThemeFromSettings();
    // One-time layout moves: the pre-rename cache tree nested twice
    // (~/.cache/booru-explorer/booru-explorer) and cache.db lived in the
    // data dir. Carry both over when the new spots are still empty.
    {
        const QString base = appCacheDir();
        const QString oldBase =
            QStandardPaths::writableLocation(QStandardPaths::CacheLocation) +
            QStringLiteral("/booru-explorer");
        for (const QString &sub :
             {QStringLiteral("thumbs"), QStringLiteral("roots"),
              QStringLiteral("favicons")}) {
            const QString dst = base + u'/' + sub;
            const QString src =
                oldBase + QStringLiteral("/booru-explorer/") + sub;
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
    // Thumbnail/full disk caches live in the platform cache dir; the
    // cache mode is applied per request so per-booru overrides work.
    const QString thumbDir = appCacheDir() + u'/' + QStringLiteral("thumbs");
    const bool thumbDisk = m_settings.cacheMode != CacheMode::None;
    m_thumbs->setCacheDir(thumbDisk ? thumbDir : QString());
    m_videoThumbs->setCacheDir(thumbDisk ? thumbDir : QString());

    auto *fileMenu = menuBar()->addMenu(tr("&File"));
    QMenu *openMenu = fileMenu->addMenu(tr("&Open"));
    connect(openMenu, &QMenu::aboutToShow, this, [this, openMenu] {
        // Rebuild on every show: last folders first (dead ones pruned),
        // then Folder… plus the full-history picker.
        openMenu->clear();
        QStringList kept;
        for (const QString &p : m_settings.history) {
            if (QDir(p).exists() && !kept.contains(p))
                kept.append(p);
        }
        kept = kept.mid(0, 10);
        if (kept != m_settings.history) {
            m_settings.history = kept;
            saveSettings(m_settings);
        }
        for (const QString &p : kept.mid(0, 5)) {
            QAction *a = openMenu->addAction(QDir(p).dirName().isEmpty()
                                                 ? p
                                                 : QDir(p).dirName());
            a->setToolTip(p);
            a->setStatusTip(p);
            connect(a, &QAction::triggered, this, [this, p] {
                m_settings.lastRoot = p;
                noteFolderOpened(m_settings, p);
                saveSettings(m_settings);
                addLocalTab(p);
            });
        }
        if (!kept.isEmpty())
            openMenu->addSeparator();
        openMenu->addAction(tr("&Folder…"), this, &MainWindow::openFolder);
        QAction *all = openMenu->addAction(tr("Open &history…"));
        all->setEnabled(!kept.isEmpty());
        connect(all, &QAction::triggered, this, [this] {
            bool ok = false;
            const QStringList items = m_settings.history;
            const QString pick = QInputDialog::getItem(
                this, tr("Open history"), tr("Folder:"), items, 0, false,
                &ok);
            if (ok && !pick.isEmpty() && QDir(pick).exists()) {
                m_settings.lastRoot = pick;
                noteFolderOpened(m_settings, pick);
                saveSettings(m_settings);
                addLocalTab(pick);
            }
        });
    });
    fileMenu->addAction(tr("Browse &Downloads"), this, [this] {
        addLocalTab(effectiveDownloadDir(m_settings));
    });
    fileMenu->addSeparator();
    fileMenu->addAction(tr("Import LoliSnatcher backup…"), this,
                        &MainWindow::importLosnBackup);
    fileMenu->addAction(tr("Backup settings…"), this,
                        &MainWindow::exportSettingsBackup);
    fileMenu->addAction(tr("Restore settings…"), this,
                        &MainWindow::importSettingsBackup);
    fileMenu->addAction(tr("Download/Add Stray…"), this,
                        &MainWindow::openStrayDialog);
    fileMenu->addAction(tr("Update downloaded sidecars…"), this,
                        &MainWindow::updateDownloadSidecars);
    fileMenu->addAction(tr("Redownload missing files…"), this,
                        &MainWindow::redownloadMissingFiles);
    fileMenu->addSeparator();
    fileMenu->addAction(QStringLiteral("&Quit"), qApp, &QApplication::quit);

    auto *tabsMenu = menuBar()->addMenu(tr("&Tabs"));
    tabsMenu->addAction(tr("&Close Folder"), this, &MainWindow::showWelcome);
    tabsMenu->addAction(tr("Start &fresh session"), this,
                        &MainWindow::startFreshSession);
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
    helpMenu->addAction(tr("&About Box"), this, &MainWindow::showAbout);
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
    auto *fullKey = new QShortcut(QKeySequence(Qt::Key_F11), this);
    fullKey->setContext(Qt::ApplicationShortcut);
    connect(fullKey, &QShortcut::activated, this,
            &MainWindow::toggleViewerFullscreen);
    auto *esc = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    esc->setContext(Qt::ApplicationShortcut);
    connect(esc, &QShortcut::activated, this, [this] {
        if (m_viewerFullscreen) {
            setViewerFullscreen(false); // first Esc leaves fullscreen
        } else if (m_pages->currentIndex() == 1) {
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
    m_omniBar = new QWidget(this);
    auto *omniLay = new QHBoxLayout(m_omniBar);
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
    m_suggester->setFavTags(favTagSet()); // starred tags outrank the rest
    m_suggester->setHistoryTags(m_settings.historyTokens); // your own recent tokens
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
    // Never visually empty: rebuildCombos() replaces this with counts
    // once data arrives ("All (12)"); same data value, same semantics.
    m_ratingBox->addItem(tr("All"), QStringLiteral("all"));
    m_categoryBox = new QComboBox(this);
    // Media-kind shortcut: writes a filetype: tag like typing it (stills
    // exclude GIF-likes + videos; ugoira/animated webp/jxl count as GIF;
    // sound is the audio-carrying container). Single-select like rating.
    // Canonical tokens (post.h): the query layer expands them to native
    // tags where verified (Danbooru), filetype: elsewhere.
    m_mediaBox = new QComboBox(this);
    m_mediaBox->setToolTip(tr("Filter by media kind (adds a filetype: tag)"));
    m_mediaBox->addItem(tr("All media"), QString());
    m_mediaBox->addItem(tr("Still images"), mediaStillsToken());
    m_mediaBox->addItem(tr("Animated GIFs"), mediaGifToken());
    m_mediaBox->addItem(tr("Anything animated"), mediaAnimatedToken());
    m_mediaBox->addItem(tr("Videos"), mediaVideoToken());
    m_mediaBox->addItem(tr("Sound"), mediaSoundToken());
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
                if (isBrowse())
                    commitSearch(); // sort rides server-side: refetch like rating
                else
                    commitFilter();
            });
    connect(m_ratingBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &MainWindow::onRatingChanged);
    connect(m_categoryBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &MainWindow::onCategoryChanged);
    connect(m_mediaBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &MainWindow::onMediaChanged);
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
        m_settings.gridBoxW = m_requestedBoxW;
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
    // Filter combos ignore the wheel until clicked (same as Preferences),
    // and size to their content: translated labels must never elide.
    auto *wheelGuard = new WheelGuard(this);
    for (QComboBox *cb : {m_ratingBox, m_categoryBox, m_mediaBox, m_sortBox, m_aspectBox}) {
        cb->installEventFilter(wheelGuard);
        cb->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    }
    m_countLabel = new QLabel(this);
    m_countLabel->setStyleSheet(QStringLiteral("color:#4a5068;"));
    m_resultLabel = new QLabel(this);
    m_resultLabel->setStyleSheet(QStringLiteral("color:#4a5068;"));
    m_dotPage = new QLabel(QStringLiteral("·"), this);
    m_dotPage->setStyleSheet(QStringLiteral("color:#4a5068;"));
    m_dotCount = new QLabel(QStringLiteral("·"), this);
    m_dotCount->setStyleSheet(QStringLiteral("color:#4a5068;"));

    // Starred tags menu (right-click any tag to star it): click a star
    // to add it to the search, like picking a suggestion.
    auto *favBtn = new QPushButton(QStringLiteral("★"), this);
    favBtn->setFixedWidth(32);
    favBtn->setToolTip(tr("Favourite tags"));
    favBtn->setCursor(Qt::PointingHandCursor);
    connect(favBtn, &QPushButton::clicked, this,
            &MainWindow::showFavTagsMenu);
    auto *histBtn = new QPushButton(this);
    histBtn->setIcon(UiIcons::icon(QStringLiteral("history")));
    histBtn->setIconSize(QSize(15, 15));
    histBtn->setFixedWidth(32);
    histBtn->setToolTip(tr("Search history"));
    histBtn->setCursor(Qt::PointingHandCursor);
    connect(histBtn, &QPushButton::clicked, this,
            &MainWindow::showSearchHistoryMenu);

    omniLay->addWidget(favBtn);
    omniLay->addWidget(histBtn);
    omniLay->addWidget(m_searchEdit, 1);
    omniLay->addWidget(m_ratingBox);
    omniLay->addWidget(m_categoryBox);
    omniLay->addWidget(m_mediaBox);
    // One-click AI-content filter: hides ai_generated/ai_created/
    // ai_assisted/ai_art everywhere (server -exclusions + client net).
    auto *aiBtn = new QPushButton(tr("AI"), this);
    aiBtn->setCheckable(true);
    aiBtn->setChecked(m_settings.hideAi);
    aiBtn->setFixedWidth(36);
    aiBtn->setToolTip(tr("Hide AI-generated content"));
    aiBtn->setCursor(Qt::PointingHandCursor);
    connect(aiBtn, &QPushButton::toggled, this, [this, aiBtn](bool on) {
        m_settings.hideAi = on;
        saveSettings(m_settings);
        aiBtn->setToolTip(on ? tr("Show AI-generated content")
                             : tr("Hide AI-generated content"));
        if (isBrowse())
            commitSearch(); // refetch with -ai_* exclusions
        else
            commitFilter();
    });
    omniLay->addWidget(m_sortBox);
    omniLay->addWidget(m_aspectBox);
    omniLay->addWidget(m_sitesBtn);
    omniLay->addWidget(aiBtn);
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
    m_sideBar = side;
    auto *sideLay = new QVBoxLayout(side);
    sideLay->setContentsMargins(4, 4, 4, 4);
    m_sideSplit = new QSplitter(Qt::Vertical, this);
    m_copyList = new QListWidget(this);
    m_charList = new QListWidget(this);
    m_artistList = new QListWidget(this);
    m_tagList = new QListWidget(this);
    m_metaList = new QListWidget(this);
    // Box order: Artists, Character, Copyright, General, Meta; native
    // custom categories (weird boorus) are appended after these.
    const QList<QPair<QString, QListWidget *>> sections = {
        {tr("ARTISTS"), m_artistList},
        {tr("CHARACTERS"), m_charList},
        {tr("COPYRIGHT"), m_copyList},
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
        m_sideHeaders.append(h);
        secLay->addWidget(h);
        secLay->addWidget(list, 1);
        list->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        sec->setMinimumHeight(52); // header stays reachable when collapsed
        m_sideSplit->addWidget(sec);
        m_sideSplit->setStretchFactor(m_sideSplit->count() - 1, 1);
        connect(list, &QListWidget::itemClicked, this, &MainWindow::onTagClicked);
        list->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(list, &QListWidget::customContextMenuRequested, this,
                &MainWindow::onTagContextMenu);
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
    // Single-scroll mode wraps the splitter: per-category scrollbars
    // disappear and one scrollbar scrolls through everything.
    m_sideScroll = new QScrollArea(this);
    m_sideScroll->setWidgetResizable(true);
    m_sideScroll->setFrameShape(QFrame::NoFrame);
    m_sideScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_sideScroll->setWidget(m_sideSplit);
    sideLay->removeWidget(m_sideSplit);
    sideLay->addWidget(m_sideScroll, 1);
    applySidebarScrollMode();

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
    connect(m_grid, &GridView::viewportResized, this, [this] {
        // Deferred: the viewport geometry settles after the view's own
        // resize handling, and snapping against the old width leaves a
        // clipped last column.
        QTimer::singleShot(0, this, &MainWindow::applyGridSnap);
    });
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
                int favAdded = 0;
                for (const Post &p : s.downloaded) {
                    const auto [site, pid] = favIdentity(p);
                    // Strays follow their own toggle (default on): they have
                    // no home tab, so favouriting is how they stay findable.
                    const bool autoFav =
                        m_settings.favAutoFav ||
                        (m_settings.strayAutoFav &&
                         p.siteName == Stray::dirName());
                    // Explicitly unfavourited rows stay out, even when a
                    // download they were part of lands afterwards.
                    if (autoFav && !m_favDb.isFavourite(site, pid) &&
                        !m_unfavVeto.contains(favVetoKey(site, pid))) {
                        if (m_favDb.add(p))
                            favAdded++;
                        touched = true;
                    }
                    // Point favourites at the file just written (or an
                    // earlier copy): they open from disk afterwards.
                    if (m_favDb.isFavourite(site, pid)) {
                        const QString tag = RemotePosts::fsTagForPost(p);
                        m_favDb.setLocalPath(
                            site, pid,
                            dest + u'/' + tag + u'/' +
                                RemotePosts::downloadFileName(tag,
                                                              p.rawJson));
                        touched = true;
                    }
                }
                // New (or newly linked) favourites must show up in any
                // open favourites tab immediately, not after a manual ↺.
                // Grid ♥/↓ + viewer heart repaint too: the landing may
                // have auto-favourited (repaintFavMarks covers the viewer
                // badges as well).
                if (touched) {
                    refreshFavTabs();
                    repaintFavMarks();
                }
                if (favAdded > 0)
                    statusBar()->showMessage(
                        tr("Favourited %1 download(s)").arg(favAdded), 6000);
                // The viewer stays open now: refresh its marks (a new
                // auto-favourite fills the heart, the link the box).
                if (m_pages->currentIndex() == 1)
                    updateViewerBadges();
                // Local tabs showing this destination are stale now: flag
                // it so landing on one rescans instead of needing F5.
                if (!s.downloaded.isEmpty()) {
                    const QString canon = QDir(dest).canonicalPath();
                    m_dirtyRoots.insert(
                        canon.isEmpty() ? QDir(dest).absolutePath() : canon);
                }
            });
    m_favDb.open(effectiveDataDir(m_settings)); // best effort: store.db lives here
    seedTagTypesOnce(effectiveTagTypesDbPath(m_settings));
    m_viewer = new ViewerPage(this);
    connect(m_viewer, &ViewerPage::back, this, &MainWindow::backToGrid);
    connect(m_viewer, &ViewerPage::fullscreenToggleRequested, this,
            &MainWindow::toggleViewerFullscreen);
    connect(m_viewer, &ViewerPage::postContextRequested, this,
            &MainWindow::showViewerContextMenu);
    m_viewer->setPlaybackDefaults(m_settings.videoMuted, m_settings.videoLoop,
                                  m_settings.volume);
    m_viewer->setMaxSide(m_settings.viewerMaxSide);
    applyBooruCreds();
    m_imgCache->setBaseDir(appCacheDir() + u'/' + QStringLiteral("full"));
    m_imgCache->setEnabled(m_settings.fullCache);
    m_viewer->setImageCache(m_imgCache);
    m_viewer->setCategoryMerges(m_settings.tagCategoryMerges);
    m_viewer->setCustomTagCategories(m_settings.tagCustomCategories);
    m_viewer->setDragLinkMode(m_settings.dragLinkMode);
    m_viewer->setThumbModel(m_model);
    connect(m_viewer, &ViewerPage::currentRowChanged, this, [this](int row) {
        if (row < 0 || row >= m_current.size())
            return;
        if (BrowserTab *t = currentTab())
            t->viewerKey = viewerKeyFor(m_current.at(row));
        // Keep the grid on the same post while the viewer covers it:
        // Esc/back then lands right where arrow-stepping left off.
        const QModelIndex idx = m_model->index(row, 0);
        if (idx.isValid())
            m_grid->scrollTo(idx, QAbstractItemView::PositionAtCenter);
        // Viewer-side stepping must refresh its marks too (counts and
        // download/cache flags belong to the row now on screen).
        updateViewerBadges();
    });
    m_viewer->setWebBackend(webDownloads);
    connect(m_viewer, &ViewerPage::variantsRequested, this,
            &MainWindow::searchVariants);
    connect(m_viewer, &ViewerPage::downloadRequested, this, [this](const Post &p) {
        // Explicit download lifts the veto (see m_unfavVeto).
        const auto [site, pid] = favIdentity(p);
        m_unfavVeto.remove(favVetoKey(site, pid));
        downloadPosts({p});
    });
    connect(m_viewer, &ViewerPage::deleteRequested, this, [this](const Post &p) {
        // Toggle side of the viewer download button: the file is on disk.
        QString abs;
        if (!p.absFile.isEmpty() && QFileInfo::exists(p.absFile))
            abs = p.absFile;
        else if (m_favDb.isOpen()) {
            const QString linked =
                m_favDb.localPaths().value(favIdentity(p));
            if (!linked.isEmpty() && QFileInfo::exists(linked))
                abs = linked;
        }
        if (abs.isEmpty())
            return; // raced a concurrent delete; button refreshes below
        const QString question =
            tr("Delete the downloaded file?\n%1\n(Favourites are kept).")
                .arg(QDir::toNativeSeparators(abs));
        if (QMessageBox::question(this, tr("Delete"), question,
                                  QMessageBox::Yes | QMessageBox::No,
                                  QMessageBox::No) != QMessageBox::Yes)
            return;
        // Hard safeguard: library archive roots are never written to.
        if (refuseArchiveWrite(abs, effectiveDownloadDir(m_settings)))
            return;
        QFile::remove(abs);
        const QFileInfo fi(abs);
        QFile::remove(fi.absolutePath() + u'/' + fi.completeBaseName() +
                      QStringLiteral(".json"));
        QFile::remove(abs + QStringLiteral(".json"));
        repaintFavMarks();
        updateViewerBadges();
        statusBar()->showMessage(tr("Deleted %1").arg(fi.fileName()), 4000);
    });
    connect(m_viewer, &ViewerPage::refreshRequested, this,
            &MainWindow::refreshPost);
    connect(m_viewer, &ViewerPage::metaMarksChanged, this,
            &MainWindow::onMetaMarksChanged);
    connect(m_viewer, &ViewerPage::probeRequested, this,
            [this](const QString &title, const QString &text) {
                RemotePosts::showProbeDialog(this, title, text);
            });
    connect(m_viewer, &ViewerPage::mediaFailed, this,
            [this](const QString &url, const QString &error) {
                // Double-clicked cards that won't play: the status line
                // names it, and clicking it opens the redacted
                // request/response like a search error does.
                const QUrl u(url);
                statusBar()->showMessage(
                    u.host().isEmpty()
                        ? tr("Media failed - click for details")
                        : tr("Media failed on %1 - click for details")
                              .arg(u.host()),
                    10000);
                setStatusProbe(
                    tr("Media load failed"),
                    RemotePosts::probeReportText(
                        RemotePosts::redactSecrets(url), 0,
                        error.toUtf8()));
                // Rotating clearances (ATF): one silent profile re-ride
                // per post, then re-show to refetch. Manual capture
                // stays the fallback when the profile holds no session.
                const QString low = error.toLower();
                const bool walled =
                    low.contains(QStringLiteral("hallenge")) ||
                    low.contains(QStringLiteral("captcha")) ||
                    low.contains(QStringLiteral("clearance")) ||
                    low.contains(QStringLiteral("wall"));
                if (!walled || !m_viewer->hasCurrent())
                    return;
                const Post cur = m_viewer->currentPost();
                if (!cur.isRemote)
                    return;
                const QString key =
                    !cur.siteKey.isEmpty()
                        ? cur.siteKey
                        : RemotePosts::keyForLabel(cur.siteName);
                const QString tag =
                    key + u'#' + QString::number(cur.id);
                if (key.isEmpty() || m_wallRefreshed.contains(tag))
                    return;
                const BooruSite *cfg = RemoteSource::siteFor(key);
                if (!cfg || cfg->baseUrl.isEmpty())
                    return;
                m_wallRefreshed.insert(tag);
                statusBar()->showMessage(
                    tr("Wall hit - refreshing clearance…"), 6000);
                CookieGrabDialog::refreshSilent(
                    cfg->baseUrl,
                    effectiveDataDir(m_settings) +
                        QStringLiteral("/webprofile"),
                    [this, key](const QStringList &fresh) {
                        if (fresh.isEmpty())
                            return; // profile holds no session: manual path
                        bool changed = false;
                        for (BooruSite &b : m_settings.boorus) {
                            if (b.key != key)
                                continue;
                            const QStringList merged =
                                RemotePosts::normalizeCookies(b.cookies +
                                                             fresh);
                            if (merged != b.cookies) {
                                b.cookies = merged;
                                changed = true;
                            }
                            break;
                        }
                        if (!changed)
                            return;
                        saveSettings(m_settings);
                        RemoteSource::setConfiguredBoorus(m_settings.boorus);
                        applyBooruCreds();
                        statusBar()->showMessage(
                            tr("Clearance refreshed - retrying media…"),
                            6000);
                        if (m_pages->currentIndex() == 1 &&
                            m_viewer->hasCurrent())
                            m_viewer->showRow(m_viewer->currentRow());
                    });
            });
    connect(m_refresher, &PostRefresher::refreshed, this,
            &MainWindow::onRefreshDone);
    connect(m_refresher, &PostRefresher::failed, this,
            &MainWindow::onRefreshFailed);
    // Background HyBooru detail hydration (summaries carry no tags).
    m_hybooruFetch = new PostRefresher(this);
    connect(m_hybooruFetch, &PostRefresher::refreshed, this,
            [this](qint64, const QJsonObject &raw) {
                m_hybooruBusy = false;
                const Post fresh = remapFresh(raw, m_hybooruTarget);
                if (!fresh.tags.trimmed().isEmpty()) {
                    auto mergeInto = [&](QVector<Post> &vec) {
                        for (Post &p : vec) {
                            if (!p.isRemote || p.id != m_hybooruCurrent.id ||
                                p.siteKey != m_hybooruCurrent.siteKey)
                                continue;
                            Post merged = fresh;
                            merged.category = p.category;
                            merged.siteKey = p.siteKey;
                            p = merged;
                        }
                    };
                    mergeInto(m_browseMerged);
                    mergeInto(m_current);
                    if (BrowserTab *t = currentTab())
                        mergeInto(t->browseMerged);
                    if (m_viewer)
                        m_viewer->applyRefreshedPost(fresh);
                    rebuildBrowseSidebar();
                }
                QTimer::singleShot(40, this,
                                   &MainWindow::pumpHybooruHydration);
            });
    connect(m_hybooruFetch, &PostRefresher::failed, this,
            [this](qint64, const QString &) {
                m_hybooruBusy = false;
                QTimer::singleShot(40, this,
                                   &MainWindow::pumpHybooruHydration);
            });
    connect(m_viewer, &ViewerPage::favouriteRequested, this,
            &MainWindow::toggleFavourite);
    connect(m_viewer, &ViewerPage::tagActivated, this,
            &MainWindow::addTagToSearch);
    connect(m_viewer, &ViewerPage::tagContextRequested, this,
            &MainWindow::showViewerTagMenu);
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
    connect(m_tabBar, &QTabBar::tabMoved, this, &MainWindow::onTabMoved);
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
    centerLay->addWidget(m_omniBar);
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
    m_welcome->setMascot(effectiveMascotPath(m_settings));
    m_welcome->setMascotOpacity(m_settings.mascotOpacity);
    m_welcome->setMascotScale(m_settings.mascotScale);
    m_welcome->setMascotMargin(m_settings.mascotMargin);
    connect(m_welcome, &WelcomePage::mascotScaleChanged, this, [this](double s) {
        m_settings.mascotScale = s;
        saveSettings(m_settings);
    });
    connect(m_welcome, &WelcomePage::openDialogRequested, this, &MainWindow::openFolder);
    connect(m_welcome, &WelcomePage::browseTabRequested, this, [this] {
        if (m_tabs.isEmpty())
            restoreTabs(); // last session first, then the new tab on top
        // Explicit default booru (Default tab setting) wins; otherwise
        // the default-boorus checklist.
        QStringList sites = m_settings.browseSites;
        if (RemoteSource::siteFor(m_settings.tabsDefault) != nullptr)
            sites = {m_settings.tabsDefault};
        if (sites.isEmpty())
            sites = m_settings.browseSites;
        addRemoteTab(sites, {});
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
    // One-click wall solver: sits with the failed-query info while a
    // search reports captcha/PoW walls. Permanent (right end): temporary
    // messages cover left-docked widgets, which would hide it exactly
    // when it is needed.
    m_wallSolveBtn = new QPushButton(this);
    m_wallSolveBtn->setVisible(false);
    m_wallSolveBtn->setCursor(Qt::PointingHandCursor);
    connect(m_wallSolveBtn, &QPushButton::clicked, this,
            &MainWindow::solveWallFromButton);
    statusBar()->addPermanentWidget(m_wallSolveBtn);
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
                updateViewerBadges();
                // Media walls join the Solve button (kept apart from search
                // walls: no auto-popup, no drop/close); a later success for
                // the same site clears them again.
                if (!sum.wallTargets.isEmpty()) {
                    for (const auto &wt : sum.wallTargets) {
                        bool known = false;
                        for (const auto &have : m_mediaWalls) {
                            if (have.first == wt.first) {
                                known = true;
                                break;
                            }
                        }
                        if (!known)
                            m_mediaWalls.append(wt);
                    }
                    updateWallButton();
                }
                for (const Post &post : sum.downloaded) {
                    if (post.siteKey.isEmpty())
                        continue;
                    bool changed = false;
                    for (int i = m_mediaWalls.size() - 1; i >= 0; --i) {
                        if (m_mediaWalls.at(i).first == post.siteKey) {
                            m_mediaWalls.removeAt(i);
                            changed = true;
                        }
                    }
                    if (changed)
                        updateWallButton();
                }
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
    m_delegate->setStaggered(m_settings.gridStaggered);
    m_grid->refreshLayout();
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
    // Missing-download offer at startup: favourited files deleted outside
    // the app get a redownload prompt once per launch (deferred past show;
    // relinkFavourites heals moved files first so only true losses prompt).
    QTimer::singleShot(0, this, [this] { relinkFavourites(); });
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
    if (m_settings.showLocalFolderInPlusMenu) {
        QAction *openAct = menu.addAction(
            UiIcons::icon(QStringLiteral("folder-open")),
            tr("Open local folder…"));
        connect(openAct, &QAction::triggered, this, &MainWindow::openFolder);
    }
    menu.addSeparator();
    // One click = one tab. Merging stays available per-tab through the
    // "Boorus…" menu, which checks any set of sources into the open tab.
    for (const BooruSite &s : m_settings.boorus) {
        if (!s.enabled)
            continue;
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
    updateSortBox();
    t->sites = sites;
    t->page = 1;
    updateTabTitle(m_curTab);
    doBrowseSearch(false);
}

// Infinite scroll: fetch the next page and append (throttled against 429s).
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

void MainWindow::relinkFavourites() {
    if (!m_favDb.isOpen())
        return;
    const QVector<FavDb::Unlinked> missing = m_favDb.unlinked();
    if (missing.isEmpty())
        return;
    // Search roots: current download folder plus legacy defaults,
    // where files downloaded by older versions live (pre-rename
    // Pictures/booru-explorer included).
    QStringList roots;
    roots << effectiveDownloadDir(m_settings);
    const QString pics = QStandardPaths::writableLocation(
        QStandardPaths::PicturesLocation);
    for (const QString &seg :
         {QStringLiteral("/box"), QStringLiteral("/booru-explorer")}) {
        const QString legacy = pics + seg;
        if (QDir(legacy).exists() && !roots.contains(legacy))
            roots << legacy;
    }
    // One listing for the whole tree (not one per missing row): fresh
    // imports can leave hundreds unlinked, and entryList per row stalls
    // tab switches. Folder-agnostic on purpose: download folders moved
    // from <Label> to <host> over versions (and label renames orphan
    // old folders), so match the _<md5> suffix in ANY site folder.
    // Same bytes = same md5, so cross-folder hits are still correct.
    QHash<QString, QString> byStem; // stem -> abs path (first wins)
    for (const QString &root : roots) {
        const QDir rootDir(root);
        if (!rootDir.exists())
            continue;
        for (const QString &siteDir :
             rootDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            const QDir dir(rootDir.absoluteFilePath(siteDir));
            for (const QFileInfo &fi : dir.entryInfoList(QDir::Files)) {
                const QString stem = fi.completeBaseName();
                // Any prefixed stem qualifies (<host>_<md5>, legacy
                // <Label>_<md5> and the underscore-leading _Stray_<md5>
                // alike).
                if (stem.contains(u'_') && !byStem.contains(stem))
                    byStem.insert(stem, fi.absoluteFilePath());
            }
        }
    }
    QHash<QPair<QString, qint64>, QString> found;
    for (const FavDb::Unlinked &u : missing) {
        const QString md5 = LosnImport::md5FromFileUrl(u.fileUrl);
        if (md5.isEmpty())
            continue;
        // <tag>_<md5>.<any ext>, in any folder
        for (auto s = byStem.begin(); s != byStem.end(); ++s) {
            if (s.key().endsWith(u'_' + md5)) {
                found.insert({u.site, u.id}, s.value());
                break;
            }
        }
    }
    const int n = m_favDb.setLocalPaths(found);
    // Regeneration: favourited strays whose file is gone but whose source
    // URL survives re-download into place (kept favourites only). Only
    // rows that previously had a file (recorded local_path now missing):
    // never-downloaded rows are not the app's to fetch unasked. Once
    // per session per row; failures simply retry next launch. Never
    // preempts an active download batch.
    QVector<Post> regen;
    if (m_downloads && !m_downloads->isActive()) {
        const QHash<QPair<QString, qint64>, QString> recorded =
            m_favDb.localPaths();
        for (const FavDb::Unlinked &u : missing) {
            if (u.site != Stray::dirName() || u.fileUrl.isEmpty() ||
                !RemotePosts::isHttpUrl(u.fileUrl))
                continue;
            if (found.contains({u.site, u.id}))
                continue;
            if (recorded.value(QPair<QString, qint64>(u.site, u.id)).isEmpty())
                continue; // never had a file: nothing was deleted
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

// Manual re-pull (File menu): favourited files deleted outside the app
// (moves already healed by relinkFavourites above). Only rows that
// previously had a file (recorded local_path now missing) qualify:
// never-downloaded favourites are not the app's to fetch unasked. Rows
// with no download source stay hidden in the grid (never marked
// downloaded) until the file returns; the prompt says so when any exist.
void MainWindow::redownloadMissingFiles() {
    if (!m_favDb.isOpen())
        return;
    const QHash<QPair<QString, qint64>, QString> paths = m_favDb.localPaths();
    // Entry behind a file URL: exact host first, then media CDNs living
    // next door (subdomain of an entry host). The entry picks the folder,
    // referer, cookies and proxy, so the re-pull lands where the first
    // download did (stamping siteKey is what makes that happen).
    auto keyForFileHost = [](const QString &fileHost) -> QString {
        if (fileHost.isEmpty())
            return {};
        const QMap<QString, QString> hosts = RemotePosts::hostKeys();
        if (hosts.contains(fileHost))
            return hosts.value(fileHost);
        for (auto it = hosts.begin(); it != hosts.end(); ++it) {
            if (fileHost.endsWith(u'.' + it.key()))
                return it.value();
        }
        return {};
    };
    // Re-pull the stored DB rows themselves (not bare reconstructions),
    // so names, tags and the sidecar come back complete.
    QVector<Post> redl;
    int unregen = 0;
    for (Post p : m_favDb.list()) {
        if (p.siteName == Stray::dirName())
            continue; // stray regen path owns these (retries next launch)
        const QString recorded = paths.value(favIdentity(p));
        if (recorded.isEmpty() || QFileInfo::exists(recorded))
            continue; // never downloaded, or still on disk
        if (p.fileUrl.isEmpty() || !RemotePosts::isHttpUrl(p.fileUrl)) {
            unregen++;
            continue;
        }
        const QString key =
            keyForFileHost(QUrl(p.fileUrl).host().toLower());
        const BooruSite *entry =
            key.isEmpty() ? nullptr : RemoteSource::siteFor(key);
        if (!entry || entry->baseUrl.isEmpty()) {
            unregen++; // URL but no configured booru to pull it through
            continue;
        }
        p.isRemote = true;
        p.siteKey = key;
        redl.append(p);
    }
    if (redl.isEmpty()) {
        statusBar()->showMessage(tr("No missing downloaded files"), 6000);
        return;
    }
    QString q = tr("%1 favourited file(s) are missing from disk. "
                   "Redownload them?")
                    .arg(redl.size());
    if (unregen > 0)
        q += tr("\n\n%1 other(s) have no download source and stay hidden.")
                 .arg(unregen);
    if (QMessageBox::question(this, tr("Missing downloads"), q,
                              QMessageBox::Yes | QMessageBox::No,
                              QMessageBox::Yes) != QMessageBox::Yes)
        return;
    downloadPosts(redl);
    statusBar()->showMessage(
        tr("Re-downloading %1 missing file(s)…").arg(redl.size()), 8000);
}

void MainWindow::setAsMascot(const Post &post) {
    if (post.absFile.isEmpty() || !QFileInfo::exists(post.absFile)) {
        statusBar()->showMessage(tr("No local file to use as mascot"), 4000);
        return;
    }
    m_settings.mascotPath = post.absFile;
    m_settings.mascotHidden = false;
    saveSettings(m_settings);
    m_welcome->setMascot(effectiveMascotPath(m_settings));
    statusBar()->showMessage(tr("Home mascot updated"), 4000);
}

void MainWindow::openStrayDialog() {
    auto *dlg = new StrayDialog(m_settings.strayAutoFav, this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    connect(dlg, &StrayDialog::autoFavChanged, this, [this](bool on) {
        m_settings.strayAutoFav = on;
        saveSettings(m_settings);
    });
    // Shared link resolution: booru post pages become tagged posts through
    // their public API (no entry is added to the booru list), direct media
    // URLs pass through unchanged. Failures are reported once, after the
    // whole batch ran.
    auto resolveLinks =
        [this](const QStringList &urls,
               const std::function<void(const QVector<Post> &)> &deliver) {
            auto *resolver = new StrayResolver(this);
            connect(resolver, &StrayResolver::progress, this,
                    [this](int done, int total, const QString &) {
                        statusBar()->showMessage(
                            tr("Reading link %1 of %2…")
                                .arg(done + 1)
                                .arg(total),
                            4000);
                    });
            connect(resolver, &StrayResolver::finished, this,
                    [this, resolver,
                     deliver](const QVector<StrayResolver::Result> &results) {
                        resolver->deleteLater();
                        QVector<Post> posts;
                        QStringList errors;
                        for (const auto &r : results) {
                            if (r.ok)
                                posts.append(r.post);
                            else
                                errors << tr("%1: %2").arg(r.sourceUrl, r.error);
                        }
                        if (!errors.isEmpty()) {
                            statusBar()->showMessage(
                                tr("Skipped %1 link(s): %2")
                                    .arg(errors.size())
                                    .arg(errors.first()),
                                12000);
                            if (posts.isEmpty())
                                QMessageBox::warning(this, tr("Stray links"),
                                                     errors.join(u'\n'));
                        }
                        deliver(posts);
                    });
            resolver->resolve(urls);
        };
    connect(dlg, &StrayDialog::downloadRequested, this,
            [this, resolveLinks](const QStringList &urls) {
                resolveLinks(urls, [this](const QVector<Post> &posts) {
                    if (!posts.isEmpty())
                        downloadPosts(posts);
                });
            });
    connect(dlg, &StrayDialog::favouriteRequested, this,
            [this, resolveLinks](const QStringList &urls) {
                resolveLinks(urls, [this](const QVector<Post> &posts) {
                    if (posts.isEmpty())
                        return;
                    if (!m_favDb.isOpen()) {
                        statusBar()->showMessage(
                            tr("Favourites are unavailable"), 6000);
                        return;
                    }
                    QVector<Post> favs;
                    for (const Post &p : posts) {
                        const auto [site, pid] = favIdentity(p);
                        if (m_unfavVeto.contains(favVetoKey(site, pid)))
                            continue;
                        favs.append(p);
                    }
                    const int added = m_favDb.addMany(favs);
                    if (added > 0) {
                        refreshFavTabs();
                        repaintFavMarks();
                        updateViewerBadges();
                        statusBar()->showMessage(
                            tr("Favourited %1 link(s) without downloading")
                                .arg(added),
                            6000);
                    } else {
                        statusBar()->showMessage(
                            tr("Nothing new to favourite"), 6000);
                    }
                });
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
    // Normalize like the rest of the tag universe: all whitespace runs
    // become single spaces (Tab/Enter must never glue itself to a tag),
    // lowercase, deduped. (The classifier, search and sidebar all key on
    // lowercase.)
    QStringList kept;
    for (const QString &t :
         dlg.tags().simplified().toLower().split(QChar::Space,
                                                  Qt::SkipEmptyParts)) {
        if (!kept.contains(t))
            kept << t;
    }
    obj[QStringLiteral("tags")] = kept.join(u' ');
    obj[QStringLiteral("stray")] = true;
    // The hand edit is the new truth: stale API buckets must not keep
    // removed tags alive. Without them, parseStray treats the flat blob as
    // untyped and the normal classifier re-buckets it after the save.
    for (const char *k : {"tags_artist", "tags_character", "tags_copyright",
                          "tags_general", "tags_meta", "stray_tags"})
        obj.remove(QString::fromLatin1(k));
    // Hard safeguard: library archive roots are never written to.
    if (refuseArchiveWrite(sidecar, effectiveDownloadDir(m_settings))) {
        statusBar()->showMessage(
            tr("Library archives are read-only"), 6000);
        return;
    }
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
    if (!refuseArchiveWrite(post.absFile, effectiveDownloadDir(m_settings))) {
        QFile touch(post.absFile);
        if (touch.open(QIODevice::ReadWrite))
            touch.setFileTime(QDateTime::currentDateTime(),
                              QFileDevice::FileModificationTime);
    }
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
                // Refresh must not blank the viewer: keep the displayed
                // frame as the pending thumb until the full file reloads.
                const QPixmap keep = m_viewer->currentPixmap();
                m_viewer->setPosts(m_current);
                if (!keep.isNull())
                    m_viewer->showThumbPreview(keep);
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

void MainWindow::exportSettingsBackup() {
    const QString path = QFileDialog::getSaveFileName(
        this, tr("Back up settings"), QDir::homePath() + QStringLiteral("/box-backup.json"),
        tr("JSON backups (*.json);;All files (*)"));
    if (path.isEmpty())
        return;
    const QJsonDocument doc(SettingsBackup::exportJson(
        m_settings, m_favDb.isOpen() ? m_favDb.list() : QVector<Post>()));
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        statusBar()->showMessage(tr("Cannot write %1").arg(path), 6000);
        return;
    }
    f.write(doc.toJson(QJsonDocument::Indented));
    statusBar()->showMessage(tr("Settings backed up to %1").arg(path), 6000);
}

void MainWindow::importSettingsBackup() {
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Restore settings"), QDir::homePath(),
        tr("JSON backups (*.json);;All files (*)"));
    if (path.isEmpty())
        return;
    QFile f(path);
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(
        f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        statusBar()->showMessage(tr("Not a settings backup: %1").arg(path),
                                  6000);
        return;
    }
    AppSettings app = m_settings;
    QVector<Post> favs;
    QString why;
    if (!SettingsBackup::importJson(doc.object(), &app, &favs, &why)) {
        statusBar()->showMessage(why.isEmpty() ? tr("Bad backup file") : why,
                                  8000);
        return;
    }
    const int added = m_favDb.isOpen() ? m_favDb.addMany(favs) : 0;
    m_settings = app;
    applyPreferences(m_settings);
    saveSettings(m_settings);
    refreshFavTabs();
    statusBar()->showMessage(
        tr("Settings restored (%1 favourites added).").arg(added), 8000);
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
    // The visible tab re-filters too. This used to activateTab(),
    // which tears the viewer down mid-look (favourite/download from
    // the post view kicked back to the grid); instead rebuild the
    // data in place and let fillModel reconcile the viewer.
    if (BrowserTab *t = currentTab(); t && t->isFavourites) {
        if (m_pages->currentIndex() == 1)
            commitFilter();
        else
            activateTab(m_curTab);
    }
    updateViewerBadges();
}

QString MainWindow::describeLastPlace() const {
    // Deliberately ignores hasLastPlace: after the last tab is closed the
    // label stays visible so clicking it can explain that there is nothing
    // left to resume (the persisted last/* keys are already dropped).
    if (m_settings.lastPlace.kind.isEmpty())
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

void MainWindow::startFreshSession() {
    // Close everything; the shrinking saves already empty the persisted
    // session, and the last place is cleared so resume stays hidden.
    while (m_tabBar->count() > 1)
        onTabClosed(1);
    m_settings.hasLastPlace = false;
    m_settings.lastPlace = AppSettings::SavedTab();
    m_settings.savedTabs.clear();
    m_settings.savedTabCurrent = 0;
    saveSettings(m_settings);
    showWelcome();
}

void MainWindow::openLastPlace() {
    // Last session first: restore every saved tab, landing on the saved
    // current one inside restoreTabs. Single-tab fallback below when
    // nothing was ever saved.
    if (m_tabs.isEmpty()) {
        if (restoreTabs())
            return;
    } else {
        const int idx =
            qBound(0, m_settings.savedTabCurrent, m_tabs.size() - 1);
        if (m_tabBar->currentIndex() == idx + 1)
            activateTab(idx); // already there: refresh in place
        else
            m_tabBar->setCurrentIndex(idx + 1); // +1: tab 0 is welcome
        return;
    }
    if (!m_settings.hasLastPlace || m_settings.lastPlace.kind.isEmpty()) {
        // Everything from the previous session was closed (or the stale
        // place did not survive): say so instead of silently doing nothing.
        QMessageBox::information(
            this, tr("Nothing to resume"),
            tr("The previous session has no open tabs left to return to."));
        m_welcome->setLastPlace({});
        return;
    }
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
        t->viewerKey = lp.viewerKey;
        t->scrollRow = lp.scrollRow;
        if (m_tabBar->currentIndex() == favIdx + 1)
            activateTab(favIdx); // already there: refresh in place
        else
            m_tabBar->setCurrentIndex(favIdx + 1);
        saveTabsNow();
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
                t->viewerKey = lp.viewerKey;
                t->scrollRow = lp.scrollRow;
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
        t->viewerKey = lp.viewerKey;
        t->scrollRow = lp.scrollRow;
        t->page = 1;
        m_tabs.append(t);
        m_tabBar->addTab(QString());
        updateTabTitle(m_tabs.size() - 1);
        m_tabBar->setCurrentIndex(m_tabs.size()); // +1: tab 0 is welcome
        saveTabsNow();
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
                t->viewerKey = lp.viewerKey;
                t->scrollRow = lp.scrollRow;
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
        t->viewerKey = lp.viewerKey;
        t->scrollRow = lp.scrollRow;
        m_tabs.append(t);
        m_tabBar->addTab(QString());
        updateTabTitle(m_tabs.size() - 1);
        m_tabBar->setCurrentIndex(m_tabs.size()); // +1: tab 0 is welcome
        saveTabsNow();
    }
}

void MainWindow::openFavTab() {
    // Every call opens a fresh tab (like remote tabs): use one per tag set,
    // e.g. different filters in different tabs. Shared state stays in the
    // DB; per-tab filters live in the tab struct.
    auto *t = new BrowserTab();
    t->isFavourites = true;
    t->sort = QStringLiteral("added"); // DB order is newest-first
    t->title = tr("Favourites");
    m_tabs.append(t);
    m_tabBar->addTab(QString());
    updateTabTitle(m_tabs.size() - 1);
    m_tabBar->setCurrentIndex(m_tabs.size());
    saveTabsNow();
}

// ── Tag refresh ──────────────────────────────────────────────

QString MainWindow::refreshReason(const Post &post) {
    // Library archives are read-only: no sidecar writes, no edits.
    if (!post.isRemote) {
        const QString dlRoot =
            QDir(effectiveDownloadDir(m_settings)).canonicalPath();
        const QString absDir =
            QDir(QFileInfo(post.absFile).absolutePath()).canonicalPath();
        if (dlRoot.isEmpty() || absDir.isEmpty() ||
            (absDir != dlRoot && !absDir.startsWith(dlRoot + u'/')))
            return tr("Library archives are read-only");
    }
    RefreshTarget t = resolveRefreshTarget(post);
    if (!t.valid()) {
        if (!post.isRemote)
            return tr("No source link (gallery-dl dumps can't refresh)");
        QString key = post.siteKey; // stamped rows name their entry
        if (key.isEmpty() || !RemoteSource::siteFor(key))
            key = RemotePosts::keyForLabel(post.siteName);
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
    m_refresher->refresh(target, ++m_refreshReq);
}

void MainWindow::onRefreshFailed(qint64 reqId, const QString &error) {
    if (reqId != m_refreshReq)
        return; // superseded: a newer refresh owns the UI now
    if (m_viewer)
        m_viewer->detailFetchFailed(m_refreshPost);
    statusBar()->showMessage(tr("Refresh failed: %1").arg(error), 8000);
}

namespace {
Post remapFresh(const QJsonObject &raw, const RefreshTarget &t) {
    // Engine-based so custom instances map like their family presets.
    using enum BooruEngine;
    switch (RemotePosts::engineForSiteKey(t.siteKey)) {
    case Danbooru:
        return RemotePosts::fromDanbooru(raw, t.baseUrl);
    case Gelbooru:
    case Safebooru:
        return RemotePosts::fromGelbooru(raw, t.baseUrl);
    case Szurubooru:
        return RemotePosts::fromSzuru(raw, t.baseUrl);
    case Hybooru:
        return RemotePosts::fromHybooru(raw, t.baseUrl);
    case Nuuru:
        return RemotePosts::fromNuuru(raw, t.baseUrl);
    case Hiddenbooru:
        return RemotePosts::fromHiddenbooru(raw, t.baseUrl);
    case Shimmie: {
        QMap<QString, QString> attrs;
        for (auto it = raw.begin(); it != raw.end(); ++it)
            attrs.insert(it.key(), it.value().toVariant().toString());
        return RemotePosts::fromShimmie(attrs, t.baseUrl);
    }
    case Moebooru:
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
    if (reqId != m_refreshReq)
        return; // superseded: an older flight landing late must not
                // remap (wrong-site data used to replace the grid row).
    const Post fresh = remapFresh(raw, m_refreshTarget);
    const Post orig = m_refreshPost;
    const QString siteKey = m_refreshTarget.siteKey;
    int tagCount = 0;

    if (orig.isRemote) {
        // In-memory update (+ downloaded copy's sidecar, if present).
        Post merged = fresh;
        merged.category = orig.category;
        merged.siteKey = siteKey; // refresh targets resolve exactly, twice
        // Repaint the open viewer even if the page-index timing made the
        // block below skip it (the first-open HyBooru tags bug).
        if (m_viewer)
            m_viewer->applyRefreshedPost(merged);
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
            // Refresh must not blank the viewer: keep the displayed
            // frame as the pending thumb until the full file reloads.
            const QPixmap keep = m_viewer->currentPixmap();
            m_viewer->setPosts(m_current);
            if (!keep.isNull())
                m_viewer->showThumbPreview(keep);
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
        // Folders moved from <Label> to <host> over versions: check the
        // host path first, then the legacy label path.
        const QString tag = RemotePosts::fsTagForPost(orig);
        const QString stem =
            RemotePosts::downloadFileName(tag, raw).split(u'.').first();
        QString sidecar = effectiveDownloadDir(m_settings) + u'/' + tag +
                          u'/' + stem + QStringLiteral(".json");
        if (!QFile::exists(sidecar) && tag != orig.siteName) {
            const QString legacyStem =
                RemotePosts::downloadFileName(orig.siteName, raw)
                    .split(u'.')
                    .first();
            sidecar = effectiveDownloadDir(m_settings) + u'/' + orig.siteName +
                      u'/' + legacyStem + QStringLiteral(".json");
        }
        if (QFile::exists(sidecar)) {
            QFile f(sidecar);
            if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
                f.write(QJsonDocument(raw).toJson(QJsonDocument::Indented)); // jq-readable
        }
    } else {
        // Local file: rewrite its sidecar, then re-parse through the normal path.
        // Archive roots (outside the download dir) are read-only like
        // everywhere else (no Delete there either): merge via a temp copy
        // so Syncthing-style mirrors never see local additions.
        const QString sidecar = localSidecarFor(orig);
        if (sidecar.isEmpty()) {
            statusBar()->showMessage(tr("Refresh: sidecar not writable"), 6000);
            return;
        }
        const QString dlRoot =
            QDir(effectiveDownloadDir(m_settings)).canonicalPath();
        const QString absDir =
            QDir(QFileInfo(orig.absFile).absolutePath()).canonicalPath();
        const bool writable =
            !dlRoot.isEmpty() &&
            (absDir == dlRoot || absDir.startsWith(dlRoot + u'/'));
        QString parsePath = sidecar;
        QTemporaryFile tmp;
        if (!writable) {
            if (!tmp.open())
                return;
            QFile src(sidecar);
            if (!src.open(QIODevice::ReadOnly))
                return;
            tmp.write(src.readAll());
            tmp.flush();
            parsePath = tmp.fileName();
        }
        // Hard safeguard: never write a sidecar into a library archive.
        // parsePath is the temp copy for archives (passes), or the real
        // sidecar in the download dir (passes). Catches any miscalc of
        // the `writable` flag above.
        if (refuseArchiveWrite(parsePath, effectiveDownloadDir(m_settings))) {
            statusBar()->showMessage(
                tr("Archive is read-only: tags refreshed for this session "
                   "only"),
                6000);
            return;
        }
        if (!writeRefreshedSidecar(parsePath, raw, siteKey)) {
            statusBar()->showMessage(tr("Refresh: sidecar not writable"), 6000);
            return;
        }
        if (!writable)
            statusBar()->showMessage(
                tr("Archive is read-only: tags refreshed for this session "
                   "only"),
                6000);
        QFile f(parsePath);
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
    if (m_viewer)
        m_viewer->stopPlayback(); // home hides the viewer: same audio leak
    m_tabBar->blockSignals(true);
    m_tabBar->setCurrentIndex(0);
    m_tabBar->blockSignals(false);
    m_stage->setCurrentIndex(0);
    setWindowTitle(QStringLiteral("Box"));
}

void MainWindow::openFolder() {
    const QString dir = QFileDialog::getExistingDirectory(
        this, tr("Open booru folder"), m_root);
    if (dir.isEmpty())
        return;
    m_settings.lastRoot = dir;
    noteFolderOpened(m_settings, dir);
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
    setWindowTitle(QStringLiteral("Box - %1").arg(root));
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
                QVector<Post> scanned = watcher->result();
                healLocalPosts(scanned);
                m_index.setPosts(scanned);
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
    // browse-tab hiding below). Native custom sections are browse-only.
    for (int i = 0; i < m_sideSplit->count(); ++i)
        m_sideSplit->widget(i)->setVisible(true);
    static const QStringList labels = {
        tr("ARTISTS"), tr("CHARACTERS"), tr("COPYRIGHT"), tr("TAGS"),
        tr("META"),
    };
    for (int i = 0; i < labels.size() && i < m_sideHeaders.size(); ++i)
        m_sideHeaders[i]->setText(labels.at(i));
    for (auto it = m_customSections.begin(); it != m_customSections.end();
         ++it)
        it->box->setVisible(false);
    const Stats s = m_index.stats();
    // User-created categories: count tags assigned to them and pull those
    // tags out of the classic top lists.
    QHash<QString, QHash<QString, int>> customCounts;
    QHash<QString, QString> customLabels;
    if (!m_settings.tagCustomCategories.isEmpty()) {
        for (const Post &p : m_index.posts()) {
            for (const QString &t :
                 p.tags.split(QChar::Space, Qt::SkipEmptyParts)) {
                const auto it =
                    m_settings.tagCustomCategories.constFind(t.toLower());
                if (it == m_settings.tagCustomCategories.constEnd())
                    continue;
                const QString key = TagCategories::normalizeKey(it.value());
                customCounts[key][t]++;
                customLabels.insert(key, it.value());
            }
        }
    }
    auto withoutAssigned = [&](QVector<QPair<QString, int>> v) {
        if (m_settings.tagCustomCategories.isEmpty())
            return v;
        v.erase(std::remove_if(v.begin(), v.end(), [&](const auto &e) {
                    return m_settings.tagCustomCategories.contains(
                        e.first.toLower());
                }),
                v.end());
        return v;
    };
    rebuildTagTab(m_copyList, withoutAssigned(s.topCopyrights),
                  QStringLiteral("copyright"));
    rebuildTagTab(m_charList, withoutAssigned(s.topCharacters),
                  QStringLiteral("character"));
    rebuildTagTab(m_artistList, withoutAssigned(s.topArtists),
                  QStringLiteral("artist"));
    rebuildTagTab(m_tagList, withoutAssigned(s.topGeneral),
                  QStringLiteral("general"));
    rebuildTagTab(m_metaList, withoutAssigned(s.topMeta),
                  QStringLiteral("meta"));
    for (auto it = customCounts.begin(); it != customCounts.end(); ++it) {
        auto sec = m_customSections.find(it.key());
        if (sec == m_customSections.end()) {
            auto *box = new QWidget(this);
            auto *secLay = new QVBoxLayout(box);
            secLay->setContentsMargins(0, 0, 0, 0);
            secLay->setSpacing(2);
            auto *h = new QLabel(this);
            h->setStyleSheet(QStringLiteral(
                "color:#4a5068;font-size:10px;font-weight:bold;"));
            secLay->addWidget(h);
            auto *list = new QListWidget(this);
            list->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
            list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
            secLay->addWidget(list, 1);
            box->setMinimumHeight(52);
            m_sideSplit->addWidget(box);
            m_sideSplit->setStretchFactor(m_sideSplit->count() - 1, 1);
            connect(list, &QListWidget::itemClicked, this,
                    &MainWindow::onTagClicked);
            list->setContextMenuPolicy(Qt::CustomContextMenu);
            connect(list, &QListWidget::customContextMenuRequested, this,
                    &MainWindow::onTagContextMenu);
            sec = m_customSections.insert(it.key(), {box, h, list});
        }
        QString label = customLabels.value(it.key(), it.key());
        label.replace(u'_', u' ');
        sec->title->setText(label.toUpper());
        QVector<QPair<QString, int>> entries;
        for (auto c = it.value().begin(); c != it.value().end(); ++c)
            entries.append({c.key(), c.value()});
        std::sort(entries.begin(), entries.end(),
                  [](const auto &a, const auto &b) {
                      return a.second > b.second;
                  });
        if (entries.size() > 30)
            entries.resize(30);
        rebuildTagTab(sec->list, entries, QStringLiteral("custom"));
        sec->box->setVisible(true);
    }
    applySidebarScrollMode();
}

void MainWindow::applySidebarScrollMode() {
    // Single-scroll: per-category scrollbars disappear, every section is
    // expanded to its full content height, and one outer scrollbar walks
    // through all of them. Per-category (default): each list scrolls
    // itself.
    const bool single = m_settings.sidebarSingleScroll;
    int totalHeight = 0;
    for (int i = 0; i < m_sideSplit->count(); ++i) {
        QWidget *box = m_sideSplit->widget(i);
        if (!box)
            continue;
        if (box->isHidden()) {
            box->setMinimumHeight(0);
            continue; // splitter skips hidden sections: don't pad them
        }
        auto *list = box->findChild<QListWidget *>();
        if (!list)
            continue;
        const int rowH = qMax(18, list->sizeHintForRow(0));
        if (single) {
            list->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
            // Header + all rows + the section's own margins. Fixed, so
            // the splitter cannot collapse sections to fit the viewport;
            // the outer scroll area owns the overflow.
            const int h = 22 + list->count() * rowH + 10;
            box->setMinimumHeight(h);
            box->setMaximumHeight(h);
            totalHeight += h;
        } else {
            list->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
            box->setMinimumHeight(52);
            if (list->count() > 0 && list->count() <= 3) {
                // Tiny categories shrink so many fit at once.
                box->setMaximumHeight(22 + list->count() * rowH + 8);
            } else {
                box->setMaximumHeight(QWIDGETSIZE_MAX);
            }
        }
    }
    if (m_sideScroll) {
        if (single) {
            // The splitter's minimum must cover every expanded section,
            // otherwise widgetResizable squeezes it to the viewport and
            // the outer scrollbar never appears.
            totalHeight +=
                m_sideSplit->handleWidth() * (m_sideSplit->count() - 1);
            m_sideSplit->setMinimumHeight(totalHeight);
            m_sideScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        } else {
            m_sideSplit->setMinimumHeight(0);
            m_sideScroll->setVerticalScrollBarPolicy(
                Qt::ScrollBarAlwaysOff);
        }
    }
    if (m_sideSplit)
        m_sideSplit->updateGeometry();
}

void MainWindow::rebuildTagTab(QListWidget *list,
                               const QVector<QPair<QString, int>> &entries,
                               const QString &bucket) {
    list->clear();
    const QColor color = UiIcons::tagBucketColor(bucket);
    for (const auto &[tag, n] : entries) {
        auto *it = new QListWidgetItem(QStringLiteral("%1  (%2)").arg(tag).arg(n));
        it->setData(Qt::UserRole, tag);
        it->setForeground(color);
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
            if (m_settings.hideAi && isAiPost(p))
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
    if (m_settings.hideAi) {
        for (const QString &ai : aiContentTags())
            tokens << QStringLiteral("-") + ai;
    }
    const QueryResult r =
        m_index.query(tokens, m_rating, m_category, m_sort, 1, INT_MAX);
    m_current = r.posts;
    fillModel();
    updateResultLabel();
    updateTabTitle(m_curTab);
}

// Downloaded posts keep their booru: host/label folders resolve
// back to the entry behind the download (null = stray/library).
void MainWindow::healLocalPosts(QVector<Post> &posts) {
    for (Post &p : posts) {
        if (p.isRemote || !p.postUrl.isEmpty() || p.id <= 0)
            continue;
        if (Stray::isStrayPath(p.absFile))
            continue;
        const BooruSite *e = RemotePosts::entryForPost(p);
        if (!e)
            continue;
        const QString url = RemotePosts::postPageUrlFor(*e, p.id);
        if (!url.isEmpty())
            p.postUrl = url;
    }
}

bool MainWindow::isPostDownloaded(
    const Post &post,
    const QHash<QPair<QString, qint64>, QString> *favLocal) {
    if (!post.isRemote) {
        // On disk under the download folder counts; plain library
        // folders are archives, not downloads. The file must actually
        // be there: a deleted-but-favourited row is not downloaded.
        if (post.absFile.isEmpty() || !QFileInfo::exists(post.absFile))
            return false;
        const QString dlRoot =
            QDir(effectiveDownloadDir(m_settings)).absolutePath();
        const QString abs = QFileInfo(post.absFile).absolutePath();
        return abs == dlRoot || abs.startsWith(dlRoot + u'/');
    }
    // Remote posts count when a favourite row links a file that is
    // still on disk (re-downloads and moved files heal via relink).
    QHash<QPair<QString, qint64>, QString> fresh;
    if (!favLocal) {
        if (!m_favDb.isOpen())
            return false;
        fresh = m_favDb.localPaths();
        favLocal = &fresh;
    }
    const auto it = favLocal->find(favIdentity(post));
    return it != favLocal->end() && !it.value().isEmpty() &&
           QFileInfo::exists(it.value());
}

void MainWindow::updateViewerBadges() {
    if (!m_viewer || !m_viewer->hasCurrent())
        return;
    const Post p = m_viewer->currentPost();
    const auto [site, pid] = favIdentity(p);
    const bool faved =
        m_favDb.isOpen() && m_favDb.isFavourite(site, pid);
    const bool downloaded = isPostDownloaded(p, nullptr);
    m_viewer->setStatusBadges(isAiPost(p), faved, downloaded);
    m_viewer->setFaved(faved);
    m_viewer->setDownloaded(downloaded);
}

void MainWindow::fillModel() {
    // Twins (remote/local copies of one post) card twice and make
    // viewer stepping stutter on "the same post", so collapse them
    // here at the single funnel every grid rebuild passes through.
    // Local rows whose file is gone can render nothing (the thumb
    // loader skips missing files below): drop them instead of leaving
    // blank cards. Remote rows always stay: they fall back to their
    // URLs (or a viewer refresh when only the post link is left).
    {
        QSet<QString> seen;
        QVector<Post> unique;
        unique.reserve(m_current.size());
        for (const Post &post : m_current) {
            if (!post.isRemote &&
                (post.absFile.isEmpty() || !QFileInfo::exists(post.absFile)))
                continue;
            const QString k = dedupeKeyFor(post);
            if (k.isEmpty() || !seen.contains(k)) {
                if (!k.isEmpty())
                    seen.insert(k);
                unique.append(post);
            }
        }
        m_current.swap(unique);
    }
    m_model->clear();
    // One query per refill (not per row): favourite links back the
    // downloaded/favourited marks below.
    const QHash<QPair<QString, qint64>, QString> favLocal =
        m_favDb.isOpen() ? m_favDb.localPaths()
                         : QHash<QPair<QString, qint64>, QString>();
    const auto favByPath = favKeysByPath(favLocal);
    // Stamp the download state once per refill: the cache policy lets
    // explicitly downloaded NSFW media persist under "SFW only" (a bare
    // favourite does not), and the thumbnail queue + viewer copies below
    // must carry the same flag.
    for (Post &p : m_current)
        p.downloaded = isPostDownloaded(p, &favLocal);
    for (const Post &p : m_current) {
        auto *item = new QStandardItem();
        QString key;
        if (p.isRemote) {
            // The info card already shows #id: display the file name here
            // so the two lines never duplicate each other. Extensionless
            // endpoints (Nuuru /file) get a synthesized readable name;
            // downloads are unaffected (they keep site_md5 naming).
            QString name = RemotePosts::displayFileName(p);
            if (name.isEmpty() && p.id)
                name = QStringLiteral("#%1").arg(p.id);
            item->setData(name, Qt::DisplayRole);
            key = QStringLiteral("url|") + RemotePosts::thumbUrlFor(p);
        } else {
            item->setData(QFileInfo(p.file).fileName(), Qt::DisplayRole);
            const QFileInfo fi(p.absFile);
            key = p.absFile + u'|' + QString::number(fi.lastModified().toMSecsSinceEpoch());
        }
        // Pixmaps window with the viewport (see requestVisibleThumbs):
        // every row starts on the shared placeholder; the ensure pass
        // below paints cached thumbs onto near-visible rows only.
        item->setData(m_placeholder, Qt::DecorationRole);
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
        // Drag payload: remote stills/GIFs carry the configured
        // direct/post URL (per-booru override included); local stills/GIFs
        // drag their own file. Videos are excluded on purpose.
        QString dragUrl;
        if (!isVideoExt(p.ext)) {
            dragUrl = p.isRemote
                          ? RemotePosts::dragUrlFor(p, m_settings.dragLinkMode)
                          : (p.absFile.isEmpty()
                                 ? QString()
                                 : QUrl::fromLocalFile(p.absFile).toString());
        }
        item->setData(dragUrl, Qt::UserRole + 20);
        // Local posts carry a hash id, not a booru number: show a dash.
        item->setData(p.isRemote && p.id ? QStringLiteral("#%1").arg(p.id)
                                         : QStringLiteral("-"),
                      Qt::UserRole + 6);
        // Origin badge: the configured entry's text + color (never the
        // engine-family code) plus its favicon. Browsed posts carry
        // their entry key; favourites and older rows resolve by label
        // instead.
        QString badge, badgeColor, badgeKey;
        QPixmap badgeFav;
        if (const BooruSite *entry = RemotePosts::entryForPost(p)) {
                badge = entry->shortName.isEmpty() ? p.siteShort
                                                   : entry->shortName;
                badgeColor = entry->badgeColor;
                badgeKey = entry->key;
                // Sync hit from memory/disk; a miss queues a fetch and
                // lands via onFaviconReady (rows update in place).
                badgeFav = m_favicons->iconFor(entry->key, entry->baseUrl)
                               .pixmap(16, 16);
            } else if (p.isRemote) {
                badge = p.siteShort;
            }
        item->setData(badge, Qt::UserRole + 7);
        item->setData(badgeColor, Qt::UserRole + 8);
        if (!badgeFav.isNull())
            item->setData(badgeFav, Qt::UserRole + 10);
        item->setData(badgeKey, Qt::UserRole + 11);
        item->setData(isAiPost(p), Qt::UserRole + 9); // grid "AI" chip
        // Comments/annotations pills (parsed where the engine exposes
        // counts/stamps/flags; viewer meta ORs in on arrival).
        item->setData(p.hasComments || p.commentCount > 0, Qt::UserRole + 14);
        item->setData(p.hasNotes, Qt::UserRole + 15);
        // Downloaded (under the download folder / linked favourite
        // file) and favourited marks, next to the AI chip.
        item->setData(isFavMarked(p, favLocal, favByPath),
                      Qt::UserRole + 12);
        item->setData(p.downloaded, Qt::UserRole + 13);
        item->setEditable(false);
        // The tooltip stays on the post page even after a download
        // links the row to disk; strays (no post URL) keep the file.
        item->setToolTip(!p.postUrl.isEmpty() ? p.postUrl : p.file);
        m_model->appendRow(item);
    }
    syncViewerToGrid();
    requestVisibleThumbs();
}

// The grid behind an open viewer changed (unfavourite, delete,
// filter): point the viewer at the same post in the new list so
// deleted rows stop showing as blank ghosts and twins stop making
// stepping stutter. Untouched when the current post sits at the
// same index, so playback never restarts for unrelated refills.
void MainWindow::syncViewerToGrid() {
    if (!m_viewer || m_pages->currentIndex() != 1 ||
        !m_viewer->hasCurrent())
        return;
    const auto ident = favIdentity(m_viewer->currentPost());
    int found = -1;
    for (int i = 0; i < m_current.size(); ++i) {
        if (favIdentity(m_current.at(i)) == ident) {
            found = i;
            break;
        }
    }
    if (found == m_viewer->currentRow())
        return; // same post, same slot: leave playback alone
    if (m_current.isEmpty()) {
        backToGrid(); // nothing left to show
        return;
    }
    const int row = found >= 0 ? found
                              : qMin(m_viewer->currentRow(), m_current.size() - 1);
    m_viewer->setPosts(m_current);
    m_viewer->showRow(row);
    const Post &p = m_current.at(row);
    const auto [site, pid] = favIdentity(p);
    m_viewer->setFaved(m_favDb.isOpen() && m_favDb.isFavourite(site, pid));
    const QString reason = refreshReason(p);
    m_viewer->setRefreshable(reason.isEmpty(), reason);
    updateViewerBadges();
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
                          const QColor &color, auto &&onClick,
                          const QString &favTag = {}) {
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
        if (!favTag.isEmpty()) {
            // Right-click stars/unstars the tag (same ★ menu as the
            // sidebar and viewer tags); left-click still removes it.
            b->setContextMenuPolicy(Qt::CustomContextMenu);
            connect(b, &QPushButton::customContextMenuRequested, this,
                    [this, favTag](const QPoint &pos) {
                        QMenu menu;
                        const bool fav = isFavTag(favTag);
                        QAction *favAct = menu.addAction(
                            fav ? tr("★ Remove from favourite tags")
                                : tr("★ Add to favourite tags"));
                        connect(favAct, &QAction::triggered, this,
                                [this, favTag] { toggleFavTag(favTag); });
                        auto *src =
                            qobject_cast<QPushButton *>(sender());
                        menu.exec(src ? src->mapToGlobal(pos)
                                      : QCursor::pos());
                    });
        }
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
    // Native custom categories (outside the blobs): one pass, consulted
    // by every pill below.
    QHash<QString, QString> customOf;
    for (const Post &p : m_current) {
        for (auto it = p.customTags.begin(); it != p.customTags.end();
             ++it) {
            for (const QString &tok : it.value()) {
                if (!customOf.contains(tok))
                    customOf.insert(tok, it.key());
            }
        }
    }
    QString pillSite;
    if (const BrowserTab *t = currentTab()) {
        for (const QString &key : t->sites) {
            if (!isVirtualSource(key)) {
                pillSite = key;
                break;
            }
        }
    }
    if (pillSite.isEmpty() && !m_current.isEmpty()) {
        const Post &first = m_current.first();
        pillSite = !first.siteKey.isEmpty()
                       ? first.siteKey
                       : RemotePosts::keyForLabel(first.siteName);
    }
    auto mergedBucket = [&](const QString &category) -> QString {
        const TagCategoryMerge *m = TagCategories::mergeFor(
            m_settings.tagCategoryMerges, pillSite, category);
        if (!m)
            return {};
        return TagCategories::classicBucket(m->target); // "" = custom
    };
    auto pillBucket = [&](const QString &tag) -> QString {
        if (m_settings.tagCustomCategories.contains(tag.toLower()))
            return QStringLiteral("custom");
        if (customOf.contains(tag)) {
            const QString cat = customOf.value(tag);
            // User merge wins, then the legacy category → bucket map.
            if (const TagCategoryMerge *m = TagCategories::mergeFor(
                    m_settings.tagCategoryMerges, pillSite, cat)) {
                const QString b = TagCategories::classicBucket(m->target);
                return b.isEmpty() ? QStringLiteral("custom") : b;
            }
            const QString mapped =
                m_settings.customBucketMap.value(cat.toLower());
            return mapped.isEmpty() ? QStringLiteral("custom") : mapped;
        }
        const QString base = !isBrowse()
                                 ? m_index.bucketOf(tag)
                                 : browseBuckets.value(
                                       tag, QStringLiteral("general"));
        if (isBrowse()) {
            const QString merged = mergedBucket(base);
            if (!merged.isEmpty())
                return merged;
            if (const TagCategoryMerge *m = TagCategories::mergeFor(
                    m_settings.tagCategoryMerges, pillSite, base)) {
                if (TagCategories::classicBucket(m->target).isEmpty())
                    return QStringLiteral("custom");
            }
        }
        return base;
    };
    for (const QString &t : m_activeTags) {
        const QString clean =
            t.startsWith(u'-') && t.size() > 1 ? t.mid(1) : t;
        // Right-click stars plain tags only (metatags like rating:/
        // filetype: would pollute the favourite-tags ranking source).
        const QString fav = clean.contains(u':') ? QString() : clean;
        // System operators read as operators: metatag hues win over the
        // bucket white (rating:g, sort:random, score:>10, ...).
        const QColor meta = UiIcons::filterTagColor(clean);
        addPill(QStringLiteral("%1 ×").arg(t), tr("Remove filter"),
                meta.isValid() ? meta : UiIcons::tagBucketColor(pillBucket(clean)),
                [this, t] {
                    m_activeTags.removeAll(t);
                    updateResultLabel(); // pill vanishes now, fetch lands later
                    commitSearch();
                },
                fav);
    }
    if (isBrowse()) {
        // Entry defaults as removable cards: one per unique token across
        // the tab's sites (minus tab-dropped ones). × drops the token for
        // this tab (kept in droppedDefaults, skipped by siteAndBase) and
        // re-searches; typing it again re-adds it as a normal tag.
        if (const BrowserTab *bt = currentTab(); bt && bt->isRemote) {
            QStringList shown;
            for (const QString &key : bt->sites) {
                const BooruSite *cfg = RemoteSource::siteFor(key);
                if (!cfg)
                    continue;
                for (const QString &tok :
                     RemotePosts::defaultTagsFor({}, cfg->defaultTags,
                                                 bt->droppedDefaults)) {
                    if (!shown.contains(tok))
                        shown << tok;
                }
            }
            for (const QString &tok : shown) {
                const QString clean =
                    tok.startsWith(u'-') && tok.size() > 1 ? tok.mid(1) : tok;
                const QColor meta = UiIcons::filterTagColor(clean);
                addPill(QStringLiteral("%1 ×").arg(tok),
                        tr("Remove default filter"),
                        meta.isValid()
                            ? meta
                            : UiIcons::tagBucketColor(pillBucket(clean)),
                        [this, tok] {
                            if (BrowserTab *live = currentTab();
                                live && live->isRemote) {
                                live->droppedDefaults.insert(tok);
                                updateResultLabel();
                                commitSearch();
                            }
                        },
                        clean.contains(u':') ? QString() : clean);
            }
        }
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
            QColor gcolor = accent;
            if (alts.size() == 1) {
                const QColor meta = UiIcons::filterTagColor(alts.first());
                if (meta.isValid())
                    gcolor = meta;
            }
            const QString singleFav =
                (alts.size() == 1 && !alts.first().contains(u':'))
                    ? QString(alts.first().startsWith(u'-') &&
                                      alts.first().size() > 1
                                  ? alts.first().mid(1)
                                  : alts.first())
                    : QString();
            addPill(label + QStringLiteral(" ×"), tr("Remove group"), gcolor,
                    [this, gi] { removeQueryGroup(gi); }, singleFav);
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
        // Looks exactly like a typed rating: tag (same rose): the box is
        // only a shortcut that sets the same filter.
        const QString tok = QStringLiteral("rating:") + m_rating;
        const QColor meta = UiIcons::filterTagColor(tok);
        addPill(tok + QStringLiteral(" ×"), tr("Clear rating"),
                meta.isValid() ? meta : accent, [this] {
                    m_rating = QStringLiteral("all");
                    updateResultLabel();
                    if (isBrowse())
                        commitSearch(); // drop it server-side too
                    else
                        commitFilter();
                });
    }
    if (!m_category.isEmpty()) {
        // Source filter dressed as a plain tag pill (there is no tag
        // syntax for origins): same look, same × behavior.
        addPill(m_category + QStringLiteral(" ×"),
                tr("Clear source"),
                UiIcons::tagBucketColor(QStringLiteral("general")), [this] {
                    m_category.clear();
                    updateResultLabel();
                    commitFilter();
                });
    }
    // Stay hidden while a post is open (grid tools invite misclicks).
    m_pillBar->setVisible(m_pills->count() > 0 &&
                          m_pages->currentIndex() != 1);
}

// Visible model rows via item geometry. indexAt() misses IconMode
// margins and the ragged right edge (returning invalid corners), which
// would widen every range to the whole model and defeat pixmap
// windowing. O(n) rect math, no painting.
void MainWindow::visibleRowRange(int &lo, int &hi) const {
    lo = m_model ? m_model->rowCount() : 0;
    hi = -1;
    if (!m_grid || !m_model || m_model->rowCount() <= 0)
        return;
    const int vh = m_grid->viewport()->height();
    for (int r = 0; r < m_model->rowCount(); ++r) {
        const QRect ir = m_grid->visualRect(m_model->index(r, 0));
        if (!ir.isValid())
            continue;
        if (ir.bottom() >= 0 && ir.top() <= vh) {
            lo = qMin(lo, r);
            hi = qMax(hi, r);
        }
    }
    if (hi < 0)
        lo = 0;
}

void MainWindow::requestVisibleThumbs() {
    if (m_current.isEmpty() || m_pages->currentIndex() != 0)
        return;
    int lo = 0, hi = -1;
    visibleRowRange(lo, hi);
    if (hi < 0)
        return; // nothing laid out yet; the next scroll tick retries
    const int overscan = 20;
    for (int row = qMax(0, lo - overscan); row <= qMin(hi + overscan, m_current.size() - 1);
         ++row) {
        const Post &p = m_current[row];
        const bool persistThumb = MediaCache::persistForPost(p);
        const bool nsfwThumb =
            MediaCache::isNsfwRating(p.rating) ||
            MediaCache::postHasBlacklistedTag(p);
        const QString siteKey = p.isRemote ? p.siteKey : QString();
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
                                       m_settings.thumbLongSide, persistThumb,
                                       nsfwThumb, siteKey);
            } else {
                // Image preview (incl. video thumbnails): fast pool decode.
                m_thumbs->request(key, thumb, 0, 0, m_settings.thumbLongSide,
                                  RemotePosts::mediaRefererFor(p), persistThumb,
                                  nsfwThumb, siteKey);
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
                                   m_settings.thumbLongSide, persistThumb,
                                   nsfwThumb, siteKey);
        else
            m_thumbs->request(key, p.absFile, mtime, fi.size(),
                              m_settings.thumbLongSide, QString(), persistThumb,
                              nsfwThumb, siteKey);
    }
    // Pixmap windowing: decoded pixmaps (the actual memory) live only on
    // near-visible rows. Rows scrolled far away drop back to the shared
    // placeholder; scrolling back repaints them from the LRU cache (or
    // re-requests). Model rows themselves stay put, so selection, menus,
    // viewer stepping and the scrollbar never notice the windowing.
    // (Role +16 is the pixmap-loaded flag - +14/+15 are the comment/note
    // pills. They collided once; every thumbnailed card wore a bubble.)
    const int keep = 60;
    for (int row = 0; row < m_model->rowCount(); ++row) {
        const QModelIndex idx = m_model->index(row, 0);
        const bool has = m_model->data(idx, Qt::UserRole + 16).toBool();
        if (row >= lo - keep && row <= hi + keep) {
            if (!has) {
                const auto it = m_thumbCache.find(
                    m_model->data(idx, Qt::UserRole + 4).toString());
                if (it != m_thumbCache.end()) {
                    m_model->setData(idx, *it, Qt::DecorationRole);
                    m_model->setData(idx, true, Qt::UserRole + 16);
                }
            }
        } else if (has) {
            m_model->setData(idx, m_placeholder, Qt::DecorationRole);
            m_model->setData(idx, false, Qt::UserRole + 16);
        }
    }
}

void MainWindow::onThumbnailReady(const QString &key, const QImage &image) {
    QPixmap pm = QPixmap::fromImage(image);
    if (pm.isNull())
        return;
    if (!m_thumbCache.contains(key)) {
        m_cacheOrder.append(key);
        if (m_cacheOrder.size() > 256)
            m_thumbCache.remove(m_cacheOrder.takeFirst());
    }
    m_thumbCache[key] = pm;
    if (m_pages->currentIndex() != 0)
        return; // grid hidden: cache only, paint on return
    // Paint near-visible matches only (indexAt() misses IconMode margins,
    // so each match checks its own rect). Far matches keep the placeholder
    // until scrolled to; the ensure pass in requestVisibleThumbs paints them.
    const int margin = m_grid ? m_grid->rowPitch() * 20 : 0;
    const int vh = m_grid ? m_grid->viewport()->height() : 0;
    for (int row = 0; row < m_model->rowCount(); ++row) {
        const QModelIndex idx = m_model->index(row, 0);
        if (m_model->data(idx, Qt::UserRole + 4).toString() != key)
            continue;
        const QRect ir = m_grid->visualRect(idx);
        if (!ir.isValid() || ir.bottom() < -margin || ir.top() > vh + margin)
            continue;
        m_model->setData(idx, pm, Qt::DecorationRole);
        m_model->setData(idx, true, Qt::UserRole + 16); // pixmap-loaded flag
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
    const auto sug = m_index.suggest(bare, 24, favTagSet());
    QVector<TagSuggestion> local;
    for (const auto &s : sug) {
        TagSuggestion t;
        t.tag = s.tag;
        t.bucket = s.bucket;
        t.count = s.count;
        local.append(t);
    }
    TagSuggester::appendHistory(local, bare, m_settings.historyTokens,
                                &m_index, 24);
    QStringList words, buckets;
    for (const auto &s : local) {
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
    // No-op rebuilds: per-keystroke identical lists must not flash the
    // popup (clear + refill repaints even when nothing changed).
    if (m_completeModel->rowCount() == tags.size()) {
        bool same = true;
        for (int i = 0; i < tags.size(); ++i) {
            const QModelIndex idx = m_completeModel->index(i, 0);
            if (idx.data(Qt::DisplayRole).toString() != tags.at(i) ||
                idx.data(Qt::UserRole + 1).toString() !=
                    (i < buckets.size() ? buckets.at(i)
                                        : QStringLiteral("general"))) {
                same = false;
                break;
            }
        }
        if (same)
            return;
    }
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
    for (int i = 0; i < m_completeModel->rowCount(); ++i) {
        const QModelIndex idx = m_completeModel->index(i, 0);
        const QString w = idx.data(Qt::DisplayRole).toString();
        // Prune stale rows: keystrokes outrun fetches, so the model may
        // still hold the previous prefix's answers. The completer
        // prefix-filters anyway, so these rows could never display - yet
        // they would eat merge cap slots and crowd out fresh matches.
        if (!w.startsWith(bare, Qt::CaseInsensitive))
            continue;
        words << w;
        buckets << idx.data(Qt::UserRole + 1).toString();
    }
    // Merge, don't replace: the instant local answers stay on top and
    // remote extras append below. An empty/error reply leaves the popup
    // exactly as it was instead of blanking good answers.
    QVector<TagSuggestion> local;
    local.reserve(words.size());
    for (int i = 0; i < words.size(); ++i) {
        TagSuggestion s;
        s.tag = words.at(i);
        s.bucket = i < buckets.size()
                       ? buckets.at(i)
                       : QStringLiteral("general");
        local.append(s);
    }
    // Union + one global relevance pass (locals win exact ties via
    // the stable rank), truncated: rich local vocabularies must not
    // starve ranked server hits.
    const QVector<TagSuggestion> merged =
        TagSuggester::merge(bare, local, suggestions, favTagSet());
    if (merged.isEmpty())
        return;
    words.clear();
    buckets.clear();
    for (const TagSuggestion &sg : merged) {
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
    // Remember committed tokens for suggestions (deduped, capped).
    for (const QString &tok : pq.andTokens)
        m_settings.historyTokens =
            QueryOr::pushRecentTag(m_settings.historyTokens, tok);
    for (const QStringList &g : pq.groups) {
        for (const QString &tok : g)
            m_settings.historyTokens =
                QueryOr::pushRecentTag(m_settings.historyTokens, tok);
    }
    m_suggester->setHistoryTags(m_settings.historyTokens);
    saveSettings(m_settings);
    m_searchQuery = canonicalQuery({}, pq.groups);
    if (m_settings.searchHistory && m_favDb.isOpen()) {
        const QString recorded = canonicalQuery(m_activeTags, pq.groups);
        if (!recorded.trimmed().isEmpty())
            m_favDb.addSearch(recorded);
    }
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
    normalizeTypedRating();
    normalizeTypedFiletype();
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
    normalizeTypedRating();
    normalizeTypedFiletype();
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
    // Accepting a suggestion (click or Enter - Enter never reaches
    // returnPressed while the popup is open) commits it like Enter:
    // pills, cleared omnibar, search. Multi-tag queries build across
    // repeated accepts, one search each.
    onSearchReturn();
}

void MainWindow::onRatingChanged() {
    m_rating = m_ratingBox->currentData().toString();
    // The box is a single-select shortcut for typing a rating: tag: a
    // fresh choice replaces any committed typed rating:/safety: tags
    // (they would AND with the box and empty everything).
    for (int i = m_activeTags.size() - 1; i >= 0; --i) {
        const QString t = m_activeTags.at(i);
        const QString low = t.toLower();
        if (isRatingToken(t) ||
            (low.startsWith(u'-') && isRatingToken(t.mid(1))))
            m_activeTags.removeAt(i);
    }
    if (isBrowse()) {
        commitSearch(); // refetch with the rating in the query + busy bar
        return;
    }
    commitFilter();
}

void MainWindow::normalizeTypedRating() {
    // A committed typed rating:/safety: tag clears the rating box to
    // "All": the tag carries the filter now, and the box must not
    // re-apply its own value on top of it.
    for (const QString &t : m_activeTags) {
        if (!isRatingToken(t))
            continue;
        if (m_rating != QStringLiteral("all")) {
            m_rating = QStringLiteral("all");
            m_ratingBox->blockSignals(true);
            m_ratingBox->setCurrentIndex(
                qMax(0, m_ratingBox->findData(m_rating)));
            m_ratingBox->blockSignals(false);
        }
        return;
    }
}

void MainWindow::onCategoryChanged() {
    m_category = m_categoryBox->currentData().toString();
    applyFilter();
}

void MainWindow::onMediaChanged() {
    // Single-select shortcut for typing a filetype: tag: a fresh choice
    // replaces any committed filetype: tokens (they would AND with the
    // box and empty everything).
    for (int i = m_activeTags.size() - 1; i >= 0; --i) {
        if (isFiletypeToken(m_activeTags.at(i)))
            m_activeTags.removeAt(i);
    }
    const QString v = m_mediaBox->currentData().toString();
    if (!v.isEmpty())
        m_activeTags.append(v);
    if (isBrowse())
        commitSearch();
    else
        commitFilter();
}

void MainWindow::syncMediaBox() {
    // The box follows the committed tags: an exact token match selects
    // the entry, anything else (none, custom) shows All media.
    if (!m_mediaBox)
        return;
    int idx = 0;
    for (const QString &t : m_activeTags) {
        if (!isFiletypeToken(t))
            continue;
        for (int i = 0; i < m_mediaBox->count(); ++i) {
            if (m_mediaBox->itemData(i).toString().compare(
                    t, Qt::CaseInsensitive) == 0) {
                idx = i;
                break;
            }
        }
        break; // first token decides (single-select box)
    }
    m_mediaBox->blockSignals(true);
    m_mediaBox->setCurrentIndex(idx);
    m_mediaBox->blockSignals(false);
}

void MainWindow::normalizeTypedFiletype() {
    // A typed filetype: tag owns the filter (the box is only a shortcut).
    syncMediaBox();
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
    m_requestedBoxW = qBound(96, w, 384);
    applyGridSnap();
}

void MainWindow::applyGridSnap() {
    if (!m_delegate || !m_grid)
        return;
    int w = m_requestedBoxW;
    if (m_settings.gridSnap) {
        const int vw = m_grid->viewport()->width();
        const int spacing = qMax(0, m_grid->spacing());
        if (vw > 0) {
            // Delegate card = box + 12px padding; the icon layout adds
            // one spacing slot on each side of every card.
            const int pitch = w + 12 + 2 * spacing;
            int cols = qMax(1, vw / qMax(1, pitch));
            int cell = (vw - cols * 2 * spacing) / cols;
            int box = qBound(96, cell - 12, 384);
            // Capped at the largest card: recompute the column count so
            // the leftover strip is as small as possible.
            if (box == 384 && cell - 12 > 384) {
                cols = qMax(1, vw / (384 + 12 + 2 * spacing));
                box = 384;
            }
            w = box;
        }
    }
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
    Theme::applyTheme(*qApp, m_settings.themeMode, QColor(m_settings.accent));
    if (m_settings.themeMode != QStringLiteral("native") &&
        !m_settings.stylesheet.isEmpty())
        Theme::applyStylesheetFile(*qApp, m_settings.stylesheet);
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
            setWindowTitle(QStringLiteral("Box - ♥ Favourites"));
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
        this, tr("About Box"),
        QStringLiteral("<h3>Box %1</h3>"
                       "<p>%2</p>"
                       "<p><a href=\"%3\">%3</a><br>"
                       "Copyright (C) 2026 Lev<br>"
                       "AGPLv3 or later</p>"
                       "<p>Built with Qt %4, miniz, Lucide icons and FFmpeg "
                       "(via Qt Multimedia)</p>")
            .arg(appVersion(), appTagline(), appRepoUrl(),
                 QString::fromLatin1(qVersion())));
}

void MainWindow::openPreferences() {
    PrefsDialog dlg(m_settings, m_favicons, this);
    connect(&dlg, &PrefsDialog::classifyNowRequested, this,
            &MainWindow::runClassifyPopup);
    connect(&dlg, &PrefsDialog::faviconsRefreshRequested, this,
            &MainWindow::refreshFavicons);
    connect(&dlg, &PrefsDialog::clearSearchHistoryRequested, this, [this] {
        if (m_favDb.isOpen())
            m_favDb.clearSearches();
    });
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
    if (m_model && m_grid) {
        for (int i = 0; i < m_current.size() && i < m_model->rowCount(); ++i) {
            if (QStandardItem *item = m_model->item(i))
                item->setData(
                    RemotePosts::dragUrlFor(m_current.at(i),
                                            m_settings.dragLinkMode),
                    Qt::UserRole + 20);
        }
    }
    // Data folder moved: reopen the favourites DB there. The cache
    // tracking DB stays in the platform cache dir (it follows the cache,
    // not the data). Databases start fresh there - existing files stay
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
    MediaCache::configure(m_settings.cacheMode, m_settings.boorus);
    MediaCache::setSfwBlacklist(m_settings.blacklist);
    setActiveExplicitTags(
        QSet<QString>(m_settings.explicitTags.begin(), m_settings.explicitTags.end()));
    RemoteSource::setConfiguredBoorus(m_settings.boorus);
    RemotePosts::setDisabledBuckets(m_settings.disabledBuckets);
    RemotePosts::setGlobalProxy(m_settings.globalProxy);
    // Starred tags retune suggestions immediately (harvested from the
    // Settings editor, not only from right-click starring).
    m_suggester->setFavTags(favTagSet());
    // The embedded browser (capture, PoW walls, onion sites) reads the
    // proxy once at startup: a changed proxy needs a restart there,
    // while searches/downloads ride it immediately.
    auto proxiesOf = [](const QList<BooruSite> &bs) {
        QStringList out;
        for (const BooruSite &b : bs)
            out << b.key + u'=' + b.proxy;
        return out;
    };
    if (old.globalProxy != m_settings.globalProxy ||
        proxiesOf(old.boorus) != proxiesOf(m_settings.boorus)) {
        if (effectiveDataDir(m_settings) == oldDataDir)
            statusBar()->showMessage(
                tr("Proxy changed. Searches and downloads use it now; "
                   "the embedded browser (captcha/PoW walls, onion sites) "
                   "picks it up after a restart."),
                10000);
    }
    if (old.manualTagTypes != m_settings.manualTagTypes) {
        // Forced overrides apply on Apply, not on restart: push the new
        // buckets into every tab's stored posts (current tab refreshes
        // its grid too). Removed entries need a rescan to fall back.
        QHash<QString, QString> manual;
        for (auto it = m_settings.manualTagTypes.begin();
             it != m_settings.manualTagTypes.end(); ++it)
            manual.insert(it.key(), it.value());
        BrowserTab *cur = currentTab();
        for (BrowserTab *tb : m_tabs) {
            if (!tb->isRemote && !tb->isFavourites && tb != cur) {
                // Snapshot-only: the shared index backs the current tab.
                applyTagBuckets(tb->localPosts, manual);
                continue;
            }
            applyManualBuckets(tb, manual, false);
        }
    }
    m_favicons->setCustomDir(effectiveDataDir(m_settings) +
                             QStringLiteral("/favicons"));
    WebApiFetcher::setProfilePath(effectiveDataDir(m_settings) +
                                  QStringLiteral("/webprofile"));
    prefetchFavicons();
    rebuildSitesMenu();
    updateSortBox();
    // Custom favicons apply immediately (same loop as fetch arrivals).
    for (int i = 0; i < m_tabs.size(); ++i) {
        if (m_tabs[i]->isRemote)
            updateTabTitle(i);
    }
    applyBooruCreds();
    applyThemeFromSettings();
    applySidebarScrollMode();
    setAspect(m_settings.aspectNum, m_settings.aspectDen);
    if (m_sizeSlider)
        m_sizeSlider->setValue(m_settings.gridBoxW);
    setBoxW(m_settings.gridBoxW);
    m_delegate->setStaggered(m_settings.gridStaggered);
    m_grid->refreshLayout();
    m_grid->setScrollRows(m_settings.gridScrollRows);
    const bool diskCachePrefs = m_settings.cacheMode != CacheMode::None;
    const QString thumbDirPrefs = appCacheDir() + u'/' + QStringLiteral("thumbs");
    m_thumbs->setCacheDir(diskCachePrefs ? thumbDirPrefs : QString());
    m_videoThumbs->setCacheDir(diskCachePrefs ? thumbDirPrefs : QString());
    m_downloads->setUgoiraFormat(m_settings.ugoiraFormat);
    m_downloads->setUgoiraFrameDelayMs(m_settings.ugoiraFrameDelayMs);
    m_viewer->setPlaybackDefaults(m_settings.videoMuted, m_settings.videoLoop,
                                  m_settings.volume);
    m_viewer->setMaxSide(m_settings.viewerMaxSide);
    m_viewer->setCategoryMerges(m_settings.tagCategoryMerges);
    m_viewer->setDragLinkMode(m_settings.dragLinkMode);
    m_viewer->setCustomTagCategories(m_settings.tagCustomCategories);
    m_welcome->setMascot(effectiveMascotPath(m_settings));
    m_welcome->setMascotOpacity(m_settings.mascotOpacity);
    m_welcome->setMascotScale(m_settings.mascotScale);
    m_welcome->setMascotMargin(m_settings.mascotMargin);
    m_imgCache->setEnabled(m_settings.fullCache);
    if (m_stage->currentIndex() == 1 && m_curTab >= 0) {
        if (isBrowse()) {
            // Re-search only when the query pipeline changed (creds, tags,
            // limit, filters): appearance/scroll edits must not refetch.
            bool sitesEqual = old.boorus.size() == m_settings.boorus.size();
            for (int i = 0; sitesEqual && i < old.boorus.size(); ++i)
                sitesEqual = old.boorus.at(i) == m_settings.boorus.at(i);
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
    if (old.tagCategoryMerges != m_settings.tagCategoryMerges ||
        old.customTagCategories != m_settings.customTagCategories ||
        old.tagCustomCategories != m_settings.tagCustomCategories) {
        if (isBrowse())
            rebuildBrowseSidebar();
        updateResultLabel();
        if (m_viewer && m_viewer->hasCurrent())
            m_viewer->showRow(m_viewer->currentRow());
    }
}

void MainWindow::closeEvent(QCloseEvent *e) {
    // Fast close (X / Alt+Q): unmap and blank the UI first so the window
    // manager responds instantly, then tear down workers without ever
    // blocking this event. Destruction-time waits (pool drains, player
    // shutdown) happen after the window is already gone.
    e->accept();
    hide();
    if (m_stage)
        m_stage->setCurrentIndex(0); // release media widgets, show home
    if (m_viewer)
        m_viewer->stopPlayback();
    cancelScan();
    m_downloads->cancel();
    m_fetcher->cancel();
    m_browseFetcher->cancel();
    m_suggester->cancel();
    if (m_refresher)
        m_refresher->cancel();
    for (RemoteSource *s : m_sources) {
        if (s)
            s->cancel();
    }
    if (m_thumbs)
        m_thumbs->cancel(); // drop queued decodes (pool wait is bounded)
    if (m_videoThumbs)
        m_videoThumbs->cancel();
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

QSet<QString> MainWindow::favTagSet() const {
    QSet<QString> out;
    for (const QString &t : m_settings.favTags) {
        if (!t.trimmed().isEmpty())
            out.insert(t.trimmed().toLower());
    }
    return out;
}

bool MainWindow::isFavTag(const QString &tag) const {
    return favTagSet().contains(tag.trimmed().toLower());
}

void MainWindow::toggleFavTag(const QString &tag) {
    const QString clean = tag.trimmed();
    if (clean.isEmpty())
        return;
    const bool wasFav = isFavTag(clean);
    if (wasFav) {
        for (int i = m_settings.favTags.size() - 1; i >= 0; --i) {
            if (m_settings.favTags.at(i).trimmed().compare(
                    clean, Qt::CaseInsensitive) == 0)
                m_settings.favTags.removeAt(i);
        }
    } else {
        m_settings.favTags.append(clean);
    }
    saveSettings(m_settings);
    m_suggester->setFavTags(favTagSet());
    statusBar()->showMessage(
        wasFav ? tr("★ %1 removed from favourite tags").arg(clean)
               : tr("★ %1 added to favourite tags").arg(clean),
        3000);
}

void MainWindow::onTagContextMenu(const QPoint &pos) {
    auto *list = qobject_cast<QListWidget *>(sender());
    if (!list)
        return;
    QListWidgetItem *it = list->itemAt(pos);
    if (!it)
        return;
    const QString tag = it->data(Qt::UserRole).toString();
    if (tag.isEmpty())
        return;
    QMenu menu;
    QAction *addAct = menu.addAction(tr("Add to search"));
    connect(addAct, &QAction::triggered, this, [this, tag] {
        if (!m_activeTags.contains(tag))
            m_activeTags.append(tag);
        commitSearch();
    });
    const bool fav = isFavTag(tag);
    QAction *favAct = menu.addAction(
        fav ? tr("★ Remove from favourite tags")
            : tr("★ Add to favourite tags"));
    connect(favAct, &QAction::triggered, this,
            [this, tag] { toggleFavTag(tag); });
    menu.exec(list->mapToGlobal(pos));
}

void MainWindow::showSearchHistoryMenu() {
    QMenu menu(this);
    const QVector<FavDb::SearchEntry> entries =
        m_favDb.isOpen() ? m_favDb.searches(30)
                         : QVector<FavDb::SearchEntry>();
    if (entries.isEmpty()) {
        QAction *none = menu.addAction(
            m_settings.searchHistory
                ? tr("No searches yet")
                : tr("Search history is off (Preferences → Browse)"));
        none->setEnabled(false);
    }
    for (const FavDb::SearchEntry &e : entries) {
        QAction *a = menu.addAction(e.query);
        a->setToolTip(tr("%1 time(s)").arg(e.count));
        connect(a, &QAction::triggered, this, [this, q = e.query] {
            // Replay replaces the current query instead of stacking.
            m_activeTags.clear();
            m_searchQuery.clear();
            m_searchEdit->setText(q);
            onSearchReturn();
        });
    }
    if (!entries.isEmpty()) {
        menu.addSeparator();
        QAction *clear = menu.addAction(tr("Clear search history"));
        connect(clear, &QAction::triggered, this, [this] {
            m_favDb.clearSearches();
            statusBar()->showMessage(tr("Search history cleared"), 4000);
        });
    }
    menu.exec(QCursor::pos());
}

void MainWindow::showFavTagsMenu() {
    QMenu menu(this);
    QStringList tags = m_settings.favTags;
    std::sort(tags.begin(), tags.end(), [](const QString &a, const QString &b) {
        return a.compare(b, Qt::CaseInsensitive) < 0;
    });
    if (tags.isEmpty()) {
        QAction *none = menu.addAction(
            tr("No favourite tags yet - right-click a tag to star it"));
        none->setEnabled(false);
    }
    // Coloured, scrollable tag list: rows wear the same bucket colours
    // as the suggest popup and pills; a real scrollbar appears past
    // ~16 rows (QMenu alone only offers scroll arrows).
    QListWidget *list = nullptr;
    if (!tags.isEmpty()) {
        // Bucket from the suggest index (server-known buckets included),
        // falling back to the local classification.
        auto bucketFor = [&](const QString &tag) -> QString {
            const auto hits = m_index.suggest(tag, 1, {});
            if (!hits.isEmpty() && !hits.first().bucket.isEmpty())
                return hits.first().bucket;
            return m_index.bucketOf(tag);
        };
        list = new QListWidget(&menu);
        list->setMaximumHeight(384);
        list->setMinimumWidth(240);
        list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        for (const QString &tag : tags) {
            if (tag.trimmed().isEmpty())
                continue;
            const QColor meta = UiIcons::filterTagColor(tag);
            const QColor color =
                meta.isValid() ? meta
                               : UiIcons::tagBucketColor(bucketFor(tag));
            auto *it = new QListWidgetItem(QStringLiteral("★ ") + tag, list);
            it->setData(Qt::UserRole, tag);
            it->setForeground(color);
        }
        auto *wa = new QWidgetAction(&menu);
        wa->setDefaultWidget(list);
        menu.addAction(wa);
        connect(list, &QListWidget::itemClicked, this,
                [this, &menu](QListWidgetItem *it) {
                    if (!it)
                        return;
                    const QString tag = it->data(Qt::UserRole).toString();
                    if (tag.isEmpty())
                        return;
                    menu.close();
                    addTagToSearch(tag);
                });
    }
    if (!tags.isEmpty()) {
        menu.addSeparator();
        QAction *manage = menu.addAction(tr("Manage favourite tags…"));
        connect(manage, &QAction::triggered, this, [this] {
            openPreferences(); // General tab lists them for removal
        });
    }
    if (QWidget *w = qobject_cast<QWidget *>(sender()))
        menu.exec(w->mapToGlobal(QPoint(0, w->height())));
    else
        menu.exec(QCursor::pos());
}

void MainWindow::onGridActivated(const QModelIndex &index) {
    if (!index.isValid())
        return;
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
    // Explicit download lifts the veto (see m_unfavVeto).
    for (const Post &p : posts) {
        const auto [site, pid] = favIdentity(p);
        m_unfavVeto.remove(favVetoKey(site, pid));
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

void MainWindow::searchVariants(const Post &post) {
    if (!post.isRemote || post.id <= 0)
        return;
    qint64 parentId = 0;
    for (const char *key : {"parent_id", "parent"}) {
        const QJsonValue v = post.rawJson.value(QLatin1String(key));
        if (v.isNull() || v.isUndefined())
            continue;
        parentId = v.toVariant().toLongLong();
        if (parentId > 0)
            break;
    }
    QStringList alts = {QStringLiteral("parent:") + QString::number(post.id),
                        QStringLiteral("child:") + QString::number(post.id),
                        QStringLiteral("id:") + QString::number(post.id)};
    if (parentId > 0) {
        alts << QStringLiteral("parent:") + QString::number(parentId)
             << QStringLiteral("id:") + QString::number(parentId);
    }
    QStringList unique;
    for (const QString &a : std::as_const(alts))
        if (!unique.contains(a))
            unique << a;
    // Family only: drop old tag cards, keep the tab's rating/blacklist.
    m_activeTags.clear();
    m_searchQuery = canonicalQuery({}, {unique});
    m_searchEdit->clear();
    backToGrid();
    commitSearch();
    statusBar()->showMessage(tr("Showing variants of #%1").arg(post.id), 5000);
}

void MainWindow::addTagToSearch(const QString &tag) {
    if (!m_activeTags.contains(tag))
        m_activeTags.append(tag);
    if (isBrowse()) {
        commitSearch();
    } else {
        rebuildSidebar();
        applyFilter();
    }
    backToGrid();
}

void MainWindow::showViewerTagMenu(const QString &tag, const QPoint &globalPos) {
    QMenu menu;
    const bool fav = isFavTag(tag);
    QAction *favAct = menu.addAction(
        fav ? tr("★ Remove from favourite tags")
            : tr("★ Add to favourite tags"));
    connect(favAct, &QAction::triggered, this,
            [this, tag] { toggleFavTag(tag); });
    menu.addSeparator();
    QAction *addAct = menu.addAction(tr("Add to search"));
    connect(addAct, &QAction::triggered, this,
            [this, tag] { addTagToSearch(tag); });
    QMenu *newTab = menu.addMenu(tr("Search in new tab"));
    // One entry per configured booru: opens a fresh browse tab limited to
    // that site with the tag already filled in.
    for (const BooruSite &site : m_settings.boorus) {
        if (!site.enabled || site.key.isEmpty())
            continue;
        const QString label =
            site.label.isEmpty() ? site.key : site.label;
        QAction *a = newTab->addAction(label);
        connect(a, &QAction::triggered, this, [this, key = site.key, tag] {
            addRemoteTab({key}, {tag});
        });
    }
    if (newTab->isEmpty())
        newTab->setEnabled(false);
    menu.exec(globalPos);
}

void MainWindow::showViewer(int row) {
    if (row >= 0 && row < m_current.size())
        if (BrowserTab *t = currentTab())
            t->viewerKey = viewerKeyFor(m_current.at(row));
    const QModelIndex idx = m_model->index(row, 0);
    m_viewer->showThumbPreview(
        qvariant_cast<QPixmap>(m_model->data(idx, Qt::DecorationRole)));
    m_viewer->setPosts(m_current);
    m_viewer->showRow(row);
    if (row >= 0 && row < m_current.size()) {
        const Post &p = m_current[row];
        const auto [site, pid] = favIdentity(p);
        m_viewer->setFaved(m_favDb.isOpen() && m_favDb.isFavourite(site, pid));
        updateViewerBadges();
        const QString reason = refreshReason(p);
        m_viewer->setRefreshable(reason.isEmpty(), reason);
    }
    if (m_omniBar)
        m_omniBar->setVisible(false);
    // Pills act on the grid too (same misclick risk as the omnibar).
    if (m_pillBar)
        m_pillBar->setVisible(false);
    m_pages->setCurrentIndex(1);
}

void MainWindow::toggleViewerFullscreen() {
    if (m_pages->currentIndex() != 1) {
        // From the grid: open the selected/current post first, then go
        // fullscreen once the viewer page is live.
        if (m_current.isEmpty())
            return;
        int row = m_grid->currentIndex().row();
        if (row < 0 || row >= m_current.size())
            row = 0;
        showViewer(row);
    }
    setViewerFullscreen(!m_viewerFullscreen);
}

void MainWindow::setViewerFullscreen(bool on) {
    if (m_viewerFullscreen == on)
        return;
    m_viewerFullscreen = on;
    if (m_viewer)
        m_viewer->setImmersive(on);
    menuBar()->setVisible(!on);
    if (m_tabBar)
        m_tabBar->setVisible(!on);
    if (m_plusBtn)
        m_plusBtn->setVisible(!on);
    if (m_omniBar)
        m_omniBar->setVisible(!on);
    if (m_sideBar)
        m_sideBar->setVisible(!on);
    if (m_pillBar)
        m_pillBar->setVisible(!on && m_pills && m_pills->count() > 0);
    statusBar()->setVisible(!on);
    if (on) {
        m_preFullscreenState = windowState();
        showFullScreen();
    } else {
        if (m_preFullscreenState & Qt::WindowMaximized)
            showMaximized();
        else
            showNormal();
        if (m_pages)
            m_pages->setCurrentIndex(1); // stay on the viewer
        if (m_viewer)
            m_viewer->setFocus();
    }
}

void MainWindow::showViewerContextMenu(const Post &post,
                                       const QPoint &globalPos) {
    QMenu menu(this);
    QAction *full = menu.addAction(
        m_viewerFullscreen ? tr("Exit fullscreen") : tr("Fullscreen"));
    full->setShortcut(QKeySequence(Qt::Key_F11));
    connect(full, &QAction::triggered, this,
            &MainWindow::toggleViewerFullscreen);
    menu.addSeparator();
    const bool hasPostLink =
        !post.postUrl.isEmpty() && RemotePosts::isHttpUrl(post.postUrl);
    const bool hasFileLink =
        !post.fileUrl.isEmpty() && RemotePosts::isHttpUrl(post.fileUrl);
    if (hasPostLink) {
        QAction *a = menu.addAction(tr("Open post page"));
        connect(a, &QAction::triggered, this, [url = post.postUrl] {
            QDesktopServices::openUrl(QUrl(url));
        });
        QAction *copyPost = menu.addAction(tr("Copy post link"));
        connect(copyPost, &QAction::triggered, this,
                [url = post.postUrl] {
                    QApplication::clipboard()->setText(url);
                });
    }
    if (hasFileLink) {
        QAction *copyFile = menu.addAction(tr("Copy media link"));
        connect(copyFile, &QAction::triggered, this,
                [url = post.fileUrl] {
                    QApplication::clipboard()->setText(url);
                });
    }
    const QString local =
        !post.absFile.isEmpty() ? post.absFile
                                : m_favDb.localPaths().value(favIdentity(post));
    if (!local.isEmpty() && QFileInfo::exists(local)) {
        QAction *copyPath = menu.addAction(tr("Copy file path"));
        connect(copyPath, &QAction::triggered, this, [local] {
            QApplication::clipboard()->setText(local);
        });
        QAction *open = menu.addAction(tr("Open file"));
        connect(open, &QAction::triggered, this, [local] {
            QDesktopServices::openUrl(QUrl::fromLocalFile(local));
        });
    }
    if (post.isRemote && hasFileLink) {
        const bool downloaded =
            !m_favDb.localPaths().value(favIdentity(post)).isEmpty();
        QAction *dl = menu.addAction(downloaded ? tr("Delete from disk…")
                                                : tr("⭳ Download"));
        connect(dl, &QAction::triggered, this, [this, post, downloaded] {
            if (!downloaded) {
                const auto [site, pid] = favIdentity(post);
                m_unfavVeto.remove(favVetoKey(site, pid));
                downloadPosts({post});
            } else {
                const QString path = m_favDb.localPaths().value(
                    favIdentity(post));
                if (path.isEmpty() || !QFileInfo::exists(path))
                    return;
                const QFileInfo fi(path);
                QFile::remove(path);
                QFile::remove(fi.absolutePath() + u'/' +
                              fi.completeBaseName() + QStringLiteral(".json"));
                QFile::remove(path + QStringLiteral(".json"));
                updateViewerBadges();
                repaintFavMarks();
                statusBar()->showMessage(tr("Deleted %1").arg(fi.fileName()),
                                        4000);
            }
        });
    }
    menu.addSeparator();
    const auto [site, pid] = favIdentity(post);
    const bool fav = m_favDb.isFavourite(site, pid);
    QAction *favAction = menu.addAction(fav ? tr("♡ Unfavourite")
                                            : tr("♥ Favourite"));
    connect(favAction, &QAction::triggered, this, [this, post, fav] {
        if (fav) {
            const auto [s, id] = favIdentity(post);
            m_favDb.remove(s, id);
            m_unfavVeto.insert(favVetoKey(s, id));
            refreshFavTabs();
            repaintFavMarks();
            updateViewerBadges();
        } else {
            toggleFavourite(post);
        }
    });
    menu.exec(globalPos);
}

void MainWindow::backToGrid() {
    // Leaving the viewer must halt playback now: audio kept going over
    // the grid (the player lives on after its page hides).
    if (m_viewerFullscreen)
        setViewerFullscreen(false);
    if (m_viewer)
        m_viewer->stopPlayback();
    m_pages->setCurrentIndex(0);
    if (m_omniBar)
        m_omniBar->setVisible(true);
    if (m_pillBar)
        m_pillBar->setVisible(m_pills->count() > 0);
    m_grid->setFocus();
    requestVisibleThumbs();
}
