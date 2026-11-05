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
    // Category labels read like words, not slugs: "entity_type" ->
    // "ENTITY TYPE" (caps, no underscores).
    auto sidebarLabel = [](const QString &raw) {
        QString s = raw;
        s.replace(u'_', u' ');
        return s.toUpper();
    };
    static const QList<QPair<QString, int>> sections = {
        {QStringLiteral("artist"), 0},    {QStringLiteral("character"), 1},
        {QStringLiteral("copyright"), 2}, {QStringLiteral("meta"), 3},
        {QStringLiteral("general"), 4},
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
    QHash<QString, QHash<QString, int>> counts = {
        {QStringLiteral("artist"), {}},   {QStringLiteral("character"), {}},
        {QStringLiteral("copyright"), {}}, {QStringLiteral("general"), {}},
        {QStringLiteral("meta"), {}},
    };
    QHash<QString, QHash<QString, int>> customCounts;
    QHash<QString, QString> customLabels;
    const QList<TagCategoryMerge> &mergeRules = m_settings.tagCategoryMerges;
    QString activeSite;
    if (const BrowserTab *t = currentTab()) {
        for (const QString &key : t->sites) {
            if (!isVirtualSource(key)) {
                activeSite = key;
                break;
            }
        }
    }
    if (activeSite.isEmpty() && !m_current.isEmpty()) {
        const Post &first = m_current.first();
        activeSite = !first.siteKey.isEmpty()
                         ? first.siteKey
                         : RemotePosts::keyForLabel(first.siteName);
    }
    auto addBucketed = [&](const QString &target, const QString &tag) {
        const auto custom =
            m_settings.tagCustomCategories.constFind(tag.toLower());
        if (custom != m_settings.tagCustomCategories.constEnd()) {
            const QString key = TagCategories::normalizeKey(custom.value());
            customCounts[key][tag]++;
            customLabels.insert(key, custom.value());
            return;
        }
        const QString bucket = TagCategories::classicBucket(target);
        if (!bucket.isEmpty()) {
            counts[bucket][tag]++;
            return;
        }
        const QString key = TagCategories::targetKey(target);
        if (key.isEmpty())
            return;
        customCounts[key][tag]++;
        customLabels.insert(key, TagCategories::displayTarget(target));
    };
    for (const Post &p : m_current) {
        const QString siteKey =
            !p.siteKey.isEmpty() ? p.siteKey
                                 : RemotePosts::keyForLabel(p.siteName);
        auto addBlob = [&](const QString &blob, const QString &bucket) {
            const TagCategoryMerge *m =
                TagCategories::mergeFor(mergeRules, siteKey, bucket);
            const QString target = m ? m->target : bucket;
            for (const QString &t :
                 blob.split(QChar::Space, Qt::SkipEmptyParts))
                addBucketed(target, t);
        };
        addBlob(p.tagsArtist, QStringLiteral("artist"));
        addBlob(p.tagsCharacter, QStringLiteral("character"));
        addBlob(p.tagsCopyright, QStringLiteral("copyright"));
        addBlob(p.tagsMeta, QStringLiteral("meta"));
        addBlob(p.tagsGeneral, QStringLiteral("general"));
        // Native custom categories (Nuuru flags, Szuru categories…): user
        // merges win, then legacy customBucketMap, then their own section.
        for (auto it = p.customTags.begin(); it != p.customTags.end();
             ++it) {
            const QString cat = it.key().toLower();
            const TagCategoryMerge *m =
                TagCategories::mergeFor(mergeRules, siteKey, cat);
            if (m) {
                for (const QString &t : it.value())
                    if (!t.trimmed().isEmpty())
                        addBucketed(m->target, t);
                continue;
            }
            const QString mapped = m_settings.customBucketMap.value(cat);
            for (const QString &t : it.value()) {
                if (t.trimmed().isEmpty())
                    continue;
                if (!mapped.isEmpty() && counts.contains(mapped)) {
                    counts[mapped][t]++;
                } else {
                    const QString key = TagCategories::normalizeKey(cat);
                    customCounts[key][t]++;
                    customLabels.insert(key,
                                        TagCategories::displayTarget(cat));
                }
            }
        }
    }
    // Renamed classic sections ("character" -> "Characters").
    {
        static const QList<QPair<int, QString>> defaults = {
            {0, QStringLiteral("artist")},
            {1, QStringLiteral("character")},
            {2, QStringLiteral("copyright")},
            {3, QStringLiteral("meta")},
            {4, QStringLiteral("general")},
        };
        for (const auto &[idx, bucket] : defaults) {
            QString label = TagCategories::labelFor(mergeRules, activeSite,
                                                    bucket);
            if (label.isEmpty()) {
                label = bucket == QStringLiteral("general")
                            ? QStringLiteral("tags")
                            : bucket;
            }
            if (idx < m_sideHeaders.size())
                m_sideHeaders[idx]->setText(sidebarLabel(label));
        }
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
    // Section visibility + fill in one pass: a bucket hidden in every
    // host, or simply empty on this page, stops rendering entirely -
    // it comes back the moment tags show up. Tiny categories shrink so
    // many sections fit at once.
    auto fillSection = [&](int idx, QListWidget *list, const QString &bucket,
                           const QVector<QPair<QString, int>> &entries) {
        QWidget *box = m_sideSplit->widget(idx);
        const bool show = !hidden.contains(bucket) && !entries.isEmpty();
        box->setVisible(show);
        box->setMaximumHeight(
            show && entries.size() <= 3 ? 18 + int(entries.size()) * 22 + 6
                                        : QWIDGETSIZE_MAX);
        rebuildTagTab(list, entries, bucket);
    };
    fillSection(0, m_artistList, QStringLiteral("artist"),
                hidden.contains(QStringLiteral("artist"))
                    ? QVector<QPair<QString, int>>()
                    : top(counts[QStringLiteral("artist")]));
    fillSection(1, m_charList, QStringLiteral("character"),
                hidden.contains(QStringLiteral("character"))
                    ? QVector<QPair<QString, int>>()
                    : top(counts[QStringLiteral("character")]));
    fillSection(2, m_copyList, QStringLiteral("copyright"),
                hidden.contains(QStringLiteral("copyright"))
                    ? QVector<QPair<QString, int>>()
                    : top(counts[QStringLiteral("copyright")]));
    fillSection(3, m_metaList, QStringLiteral("meta"),
                hidden.contains(QStringLiteral("meta"))
                    ? QVector<QPair<QString, int>>()
                    : top(counts[QStringLiteral("meta")]));
    fillSection(4, m_tagList, QStringLiteral("general"),
                top(counts[QStringLiteral("general")]));
    // Unmapped custom categories render as their own sections (same list
    // behaviour as the fixed five: click filters, right-click stars).
    static constexpr int kMaxCustomSections = 8;
    int shown = 0;
    for (auto it = customCounts.begin(); it != customCounts.end(); ++it) {
        if (shown >= kMaxCustomSections)
            break;
        auto sec = m_customSections.find(it.key());
        if (sec == m_customSections.end()) {
            auto *box = new QWidget(this);
            auto *secLay = new QVBoxLayout(box);
            secLay->setContentsMargins(0, 0, 0, 0);
            secLay->setSpacing(2);
            auto *h = new QLabel(
                sidebarLabel(customLabels.value(it.key(), it.key())), this);
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
        sec->title->setText(
            sidebarLabel(customLabels.value(it.key(), it.key())));
        rebuildTagTab(sec->list, top(it.value()), QStringLiteral("custom"));
        sec->box->setVisible(true);
        // Tiny custom categories shrink like the fixed five.
        const int n = qMin<int>(it.value().size(), 30);
        sec->box->setMaximumHeight(n <= 3 && n > 0
                                       ? 18 + n * 22 + 6
                                       : QWIDGETSIZE_MAX);
        ++shown;
    }
    for (auto it = m_customSections.begin(); it != m_customSections.end();
         ++it) {
        if (!customCounts.contains(it.key()))
            it->box->setVisible(false);
    }
    applySidebarScrollMode(); // override heights for the scroll mode
}

