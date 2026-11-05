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

