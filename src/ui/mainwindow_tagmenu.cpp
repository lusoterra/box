#include "mainwindow.h"
#include "tagcategories.h"
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
        mergeActiveTags({tag}); // adds it, or flips an existing "-tag"
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
        for (qsizetype i = m_settings.favTags.size() - 1; i >= 0; --i) {
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
        wasFav ? tr("Removed %1 from favourite tags").arg(clean)
               : tr("Added %1 to favourite tags").arg(clean),
        3000);
}

void MainWindow::categorizeTagManually(const QString &tag,
                                       const QString &bucket) {
    const QString clean = tag.trimmed().toLower();
    if (clean.isEmpty() || !TagTypes::isValidBucket(bucket))
        return;
    m_settings.manualTagTypes.insert(clean, bucket);
    saveSettings(m_settings);
    BrowserTab *t = currentTab();
    if (t && !t->isHydrus) {
        QHash<QString, QString> one;
        one.insert(clean, bucket);
        applyManualBuckets(t, one, false);
        refreshViewerTags(t);
    }
    statusBar()->showMessage(
        tr("Pinned %1 → %2").arg(clean, bucket), 4000);
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
    const QString sourceCategory = it->data(Qt::UserRole + 1).toString();
    QMenu menu;
    QAction *addAct = menu.addAction(tr("Add to search"));
    connect(addAct, &QAction::triggered, this, [this, tag] {
        mergeActiveTags({tag});
        commitSearch();
    });
    const bool fav = isFavTag(tag);
    QAction *favAct = menu.addAction(
        UiIcons::icon(QStringLiteral("star")),
        fav ? tr("Remove from favourite tags")
            : tr("Add to favourite tags"));
    connect(favAct, &QAction::triggered, this,
            [this, tag] { toggleFavTag(tag); });
    if (!sourceCategory.isEmpty()) {
        QAction *recatAct = menu.addAction(tr("Recategorise…"));
        connect(recatAct, &QAction::triggered, this, [this, tag, sourceCategory] {
            const QStringList targets = {tr("Artist"), tr("Character"),
                                         tr("Copyright"), tr("Meta"),
                                         tr("General")};
            bool ok = false;
            const QString choice = QInputDialog::getItem(
                this, tr("Recategorise tag"), tr("Move \"%1\" to:").arg(tag),
                targets, 1, false, &ok);
            if (!ok || choice.isEmpty())
                return;
            QString bucket = choice.toLower();
            // Map translated labels back to canonical buckets.
            if (bucket == tr("artist").toLower())
                bucket = QStringLiteral("artist");
            else if (bucket == tr("character").toLower())
                bucket = QStringLiteral("character");
            else if (bucket == tr("copyright").toLower())
                bucket = QStringLiteral("copyright");
            else if (bucket == tr("meta").toLower())
                bucket = QStringLiteral("meta");
            else if (bucket == tr("general").toLower())
                bucket = QStringLiteral("general");
            // canonical fallback already lower
            QString siteKey;
            if (BrowserTab *t = currentTab()) {
                for (const QString &key : t->sites) {
                    if (isVirtualSource(key))
                        continue;
                    siteKey = key;
                    break;
                }
            }
            if (siteKey.isEmpty() && !m_current.isEmpty()) {
                const Post &first = m_current.first();
                siteKey = !first.siteKey.isEmpty()
                              ? first.siteKey
                              : RemotePosts::keyForLabel(first.siteName);
            }
            TagCategoryMerge m;
            m.siteKey = siteKey;
            m.sources = {sourceCategory};
            m.target = bucket;
            // Replace any existing rule for the same scope/source.
            for (qsizetype i = m_settings.tagCategoryMerges.size() - 1;
                 i >= 0; --i) {
                const TagCategoryMerge &r = m_settings.tagCategoryMerges.at(i);
                if (r.siteKey.compare(siteKey, Qt::CaseInsensitive) == 0 &&
                    r.sources.contains(sourceCategory,
                                       Qt::CaseInsensitive))
                    m_settings.tagCategoryMerges.removeAt(i);
            }
            m_settings.tagCategoryMerges.append(m);
            saveSettings(m_settings);
            m_viewer->setCategoryMerges(m_settings.tagCategoryMerges);
            // Rebuild in place, not just the section: the grid, pill colours
            // and status counts all read the merges now. A source category
            // left with no tags disappears with this same reload.
            applyFilter();
            if (!isBrowse())
                rebuildBrowseSidebar();
            statusBar()->showMessage(
                tr("Recategorised %1 → %2").arg(tag, choice), 3000);
        });
    }
    QMenu *categorize = menu.addMenu(tr("Categorize tag as"));
    const QList<QPair<QString, QString>> buckets = {
        {tr("Artist"), QStringLiteral("artist")},
        {tr("Character"), QStringLiteral("character")},
        {tr("Copyright"), QStringLiteral("copyright")},
        {tr("Meta"), QStringLiteral("meta")},
        {tr("General"), QStringLiteral("general")}};
    for (const auto &[label, key] : buckets) {
        QAction *a = categorize->addAction(label);
        connect(a, &QAction::triggered, this,
                [this, tag, key] { categorizeTagManually(tag, key); });
    }
    menu.exec(list->mapToGlobal(pos));
}

