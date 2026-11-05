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
    auto addPill = [this](const QString &pillText, const QString &tip,
                          const QColor &color, auto &&onClick,
                          const QString &favTag = {}) {
        auto *b = new QPushButton(pillText, this);
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
