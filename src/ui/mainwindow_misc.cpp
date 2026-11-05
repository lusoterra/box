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

