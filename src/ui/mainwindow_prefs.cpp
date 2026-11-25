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
#include "migrations/runner.h"
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

void MainWindow::showAbout() {
    QMessageBox::about(
        this, tr("About Box"),
        QStringLiteral("<h3>Box %1</h3>"
                       "<p>%2</p>"
                       "<p><a href=\"%3\">%3</a><br>"
                       "Copyright (C) 2026 Lev<br>"
                       "AGPLv3 or later</p>"
                       "<p>Contact: <a href=\"mailto:lev@lusosterone.net\">"
                       "lev@lusosterone.net</a><br>"
                       "PGP: <code>13EA3CFB9ABC093823509BD6"
                       "68C62637ED987692</code></p>"
                       "<p>Built with Qt %4, miniz, Lucide icons and FFmpeg "
                       "(via Qt Multimedia)</p>")
            .arg(appVersion(), appTagline(), appRepoUrl(),
                 QString::fromLatin1(qVersion())));
}

void MainWindow::showKeyboardHelp() {
    QMessageBox box(this);
    box.setWindowTitle(tr("Keyboard shortcuts"));
    box.setTextFormat(Qt::PlainText);
    box.setText(
        tr("Grid\n"
           "  Enter: open post (single selection only)\n"
           "  Space: select / deselect post\n"
           "  Arrows: move through posts\n"
           "  Ctrl+D: download current / selected posts\n"
           "  Ctrl+O: open post in browser\n"
           "  Ctrl+Shift+O: open media in default viewer\n"
           "  / or Ctrl+L: focus search\n"
           "  Esc: clear filters\n"
           "  F5 / Ctrl+R: reload\n"
           "\n"
           "Post view\n"
           "  Left / Right: previous / next post\n"
           "  Backspace / Esc: back to grid\n"
           "  F: favourite\n"
           "  Ctrl+D: download post\n"
           "  Ctrl+O: open post in browser\n"
           "  Ctrl+Shift+O: open media in default viewer\n"
           "  F11: fullscreen\n"
           "  Mouse wheel: zoom\n"
           "\n"
           "Tabs\n"
           "  Ctrl+T: new default tab\n"
           "  Ctrl+W: close tab\n"
           "  Ctrl+Shift+T: reopen last closed tab\n"
           "\n"
           "Window\n"
           "  Ctrl+,: Preferences\n"
           "  F1: this help"));
    box.setInformativeText(tr("More help and documentation: %1")
                               .arg(appRepoUrl()));
    QPushButton *docs =
        box.addButton(tr("Documentation…"), QMessageBox::ActionRole);
    box.addButton(QMessageBox::Close);
    box.exec();
    if (box.clickedButton() == docs)
        QDesktopServices::openUrl(QUrl(appRepoUrl()));
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
    // Restart bypasses closeEvent(): stop the sockets but keep the download
    // journal, so the relaunched instance offers to resume the batch.
    if (m_downloads)
        m_downloads->shutdown();
    QProcess::startDetached(QCoreApplication::applicationFilePath(),
                            QCoreApplication::arguments().mid(1));
    QCoreApplication::quit();
}

void MainWindow::applyPreferences(const AppSettings &app) {
    const QString oldDataDir = effectiveDataDir(m_settings);
    const AppSettings old = m_settings;
    auto mediaAuthOf = [](const QList<BooruSite> &boorus) {
        QStringList out;
        for (const BooruSite &b : boorus)
            out << b.key + u'=' + b.cookies.join(u';') + u'|' + b.user +
                       u'|' + b.secret + u'|' + b.proxy + u'|' + b.baseUrl;
        return out;
    };
    const QStringList oldMediaAuth = mediaAuthOf(old.boorus);
    const bool classifyBackendChanged =
        classifyBackendKey(old.classifyBackend) !=
            classifyBackendKey(app.classifyBackend) ||
        old.classifyCustomUrl != app.classifyCustomUrl ||
        old.classifyCustomFormat != app.classifyCustomFormat;
    m_settings = app;
    if (classifyBackendChanged) {
        if (m_fetcher && m_fetcher->isActive())
            m_fetcher->cancel();
        if (m_browseFetcher && m_browseFetcher->isActive())
            m_browseFetcher->cancel();
    }
    setAppCacheDirOverride(m_settings.cacheDir);
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
        // A moved/copied-in data dir may hold pre-migration DBs: run the
        // one-time upgrades for the new location (no-ops when current).
        MigrationRunner::runPending(this, m_settings);
        m_favDb.open(effectiveDataDir(m_settings));
        // Tag vocabulary follows the data folder too: suggestions keep
        // their counts in the new location (fresh DB until one is moved).
        m_tagCache.open(TagCountCache::dbPath(effectiveDataDir(m_settings)));
        refreshFavTabs();
        statusBar()->showMessage(
            tr("Data folder moved. Existing databases stay where they were "
               "unless you move them over."), 8000);
    }
    CacheJanitor::configure(
        appCacheDir(), qint64(m_settings.cacheLimitMB) * 1024 * 1024);
    MediaCache::configure(m_settings.cacheMode, m_settings.boorus);
    MediaCache::setSfwBlacklist(m_settings.blacklist);
    // Hydrus storage moved: close the old connection and let the next
    // activation reopen the new one. An open hydrus tab reloads now.
    if (effectiveDataDir(m_settings) != oldDataDir ||
        old.hydrusDir != m_settings.hydrusDir ||
        old.hydrusDb != m_settings.hydrusDb) {
        m_hydrusDb.close();
        m_hydrusOrphanScanDone = false;
        if (BrowserTab *t = currentTab(); t && t->isHydrus)
            loadHydrusTab(t);
    }
    if (old.hydrusUrl != m_settings.hydrusUrl ||
        old.hydrusAccessKey != m_settings.hydrusAccessKey ||
        old.hydrusProxy != m_settings.hydrusProxy ||
        old.hydrusMinTagPosts != m_settings.hydrusMinTagPosts) {
        // Let a fixed/re-enabled hydd extension be retried.
        m_hydrusCountsUnavailable = false;
        m_hydrusCountsWhy.clear();
    }
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
            if (!tb->isRemote && !tb->isFavourites && !tb->isHydrus &&
                tb != cur) {
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
    applyClassifySettings();
    // Capturing a captcha/clearance in Preferences must revive the cards
    // that already failed: failures are never cached, so re-requesting the
    // visible keys rides the new cookies/browser profile immediately.
    if (mediaAuthOf(m_settings.boorus) != oldMediaAuth &&
        m_stage && m_stage->currentIndex() == 1) {
        if (m_thumbTimer)
            m_thumbTimer->start(0);
        if (m_pages && m_pages->currentIndex() == 1 && m_viewer &&
            m_viewer->hasCurrent())
            m_viewer->showRow(m_viewer->currentRow());
    }
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
                                  m_settings.volume, m_settings.videoPitch);
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

