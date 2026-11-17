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
    const QString sourceCategory = it->data(Qt::UserRole + 1).toString();
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
            for (int i = m_settings.tagCategoryMerges.size() - 1; i >= 0; --i) {
                const TagCategoryMerge &r = m_settings.tagCategoryMerges.at(i);
                if (r.siteKey.compare(siteKey, Qt::CaseInsensitive) == 0 &&
                    r.sources.contains(sourceCategory,
                                       Qt::CaseInsensitive))
                    m_settings.tagCategoryMerges.removeAt(i);
            }
            m_settings.tagCategoryMerges.append(m);
            saveSettings(m_settings);
            m_viewer->setCategoryMerges(m_settings.tagCategoryMerges);
            rebuildBrowseSidebar();
            statusBar()->showMessage(
                tr("Recategorised %1 → %2").arg(tag, choice), 3000);
        });
    }
    menu.exec(list->mapToGlobal(pos));
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

