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

Post remapFresh(const QJsonObject &raw, const RefreshTarget &t);


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
