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

void MainWindow::showWallMenu(const QStringList &wallSites) {
    BrowserTab *t = currentTab();
    if (!t || !t->isRemote || wallSites.isEmpty())
        return;
    QMenu menu(this);
    for (const QString &key : wallSites) {
        const BooruSite *cfg = RemoteSource::siteFor(key);
        const QString label = cfg ? cfg->label : key;
        QAction *solve = menu.addAction(
            tr("Solve %1 challenge in browser…").arg(label));
        connect(solve, &QAction::triggered, this,
                [this, key] {
                    solveWallInBrowser(key, m_wallKinds.value(key),
                                       wallStartUrl(key));
                });
    }
    menu.addSeparator();
    const int liveSites = t->sites.size();
    if (liveSites <= 1) {
        QAction *close = menu.addAction(tr("Close tab"));
        connect(close, &QAction::triggered, this, [this] {
            if (m_curTab >= 0)
                onTabClosed(m_curTab + 1); // bar index 0 is welcome
        });
    } else if (wallSites.size() == 1) {
        const BooruSite *cfg = RemoteSource::siteFor(wallSites.first());
        QAction *drop = menu.addAction(
            tr("Search without %1").arg(cfg ? cfg->label : wallSites.first()));
        connect(drop, &QAction::triggered, this,
                [this, wallSites] { dropWallSites(wallSites); });
    } else {
        QAction *drop = menu.addAction(tr("Search without blocked boorus"));
        connect(drop, &QAction::triggered, this,
                [this, wallSites] { dropWallSites(wallSites); });
    }
    menu.exec(QCursor::pos());
}

void MainWindow::solveWallInBrowser(const QString &siteKey,
                                      const QString &wallKind,
                                      const QUrl &startUrl) {
    int idx = -1;
    for (int i = 0; i < m_settings.boorus.size(); ++i) {
        if (m_settings.boorus.at(i).key == siteKey) {
            idx = i;
            break;
        }
    }
    if (idx < 0)
        return;
    BooruSite &entry = m_settings.boorus[idx];
    if (entry.baseUrl.isEmpty()) {
        statusBar()->showMessage(
            tr("%1 needs an instance URL first").arg(entry.label), 6000);
        return;
    }
    QUrl start = startUrl;
    // PoW/captcha challenges only fire where site JS runs: an API JSON
    // URL (ATF 449s, soybooru pow_required) loads bare data with no
    // worker and no challenge, so start at the entry root instead and
    // let the guidance send the user searching in-page. Media/file URLs
    // keep the failing address (the wall lives on that host).
    if ((wallKind == QStringLiteral("pow") ||
         wallKind == QStringLiteral("captcha")) &&
        start.isValid()) {
        const QString path = start.path().toLower();
        if (path.contains(QStringLiteral("/api/")) ||
            path.endsWith(QStringLiteral(".json")))
            start = QUrl(entry.baseUrl);
    }
    CookieGrabDialog dlg(entry.label, entry.baseUrl,
                         effectiveDataDir(m_settings) +
                             QStringLiteral("/webprofile"),
                         this, wallKind, start);
    if (dlg.exec() == QDialog::Accepted && !dlg.cookies().isEmpty()) {
        // A solved media wall clears too (same entry, file-host page).
        for (int i = m_mediaWalls.size() - 1; i >= 0; --i) {
            if (m_mediaWalls.at(i).first == siteKey)
                m_mediaWalls.removeAt(i);
        }
        updateWallButton();
        entry.cookies = dlg.cookies();
        saveSettings(m_settings);
        RemoteSource::setConfiguredBoorus(m_settings.boorus);
        applyBooruCreds();
        statusBar()->showMessage(
            tr("✓ %1: %2 cookie(s) captured - retrying search")
                .arg(entry.label)
                .arg(entry.cookies.size()),
            6000);
        if (BrowserTab *t = currentTab(); t && t->isRemote)
            doBrowseSearch(false);
    }
}

QUrl MainWindow::wallStartUrl(const QString &siteKey) const {
    if (m_wallFailUrls.contains(siteKey))
        return m_wallFailUrls.value(siteKey);
    for (const auto &wt : m_mediaWalls) {
        if (wt.first == siteKey)
            return QUrl(wt.second);
    }
    return {};
}

static QStringList wallKeysOf(const QStringList &search,
                              const QList<QPair<QString, QString>> &media) {
    QStringList keys = search;
    for (const auto &wt : media) {
        if (!keys.contains(wt.first))
            keys.append(wt.first);
    }
    return keys;
}

void MainWindow::updateWallButton() {
    if (!m_wallSolveBtn)
        return;
    const QStringList keys = wallKeysOf(m_wallSites, m_mediaWalls);
    if (keys.isEmpty()) {
        m_wallSolveBtn->setVisible(false);
        return;
    }
    if (keys.size() == 1) {
        const BooruSite *cfg = RemoteSource::siteFor(keys.first());
        m_wallSolveBtn->setText(
            tr("Solve %1 in browser…")
                .arg(cfg ? cfg->label : keys.first()));
    } else {
        m_wallSolveBtn->setText(
            tr("Solve walls (%1)…").arg(keys.size()));
    }
    m_wallSolveBtn->setToolTip(
        tr("Open the capture browser: grab cookies, ride PoW, solve "
           "the challenge"));
    m_wallSolveBtn->setVisible(true);
}

void MainWindow::solveWallFromButton() {
    const QStringList keys = wallKeysOf(m_wallSites, m_mediaWalls);
    if (keys.isEmpty())
        return;
    if (keys.size() == 1) {
        solveWallInBrowser(keys.first(), m_wallKinds.value(keys.first()),
                           wallStartUrl(keys.first()));
        return;
    }
    QMenu menu(this);
    for (const QString &key : keys) {
        const BooruSite *cfg = RemoteSource::siteFor(key);
        QAction *solve = menu.addAction(
            tr("Solve %1 challenge in browser…")
                .arg(cfg ? cfg->label : key));
        connect(solve, &QAction::triggered, this,
                [this, key] {
                    solveWallInBrowser(key, m_wallKinds.value(key),
                                       wallStartUrl(key));
                });
    }
    menu.exec(QCursor::pos());
}

void MainWindow::dropWallSites(const QStringList &siteKeys) {
    BrowserTab *t = currentTab();
    if (!t || !t->isRemote)
        return;
    for (const QString &key : siteKeys)
        t->sites.removeAll(key);
    if (t->sites.isEmpty()) {
        // Nothing left to search: close instead of idling on an empty tab.
        if (m_curTab >= 0)
            onTabClosed(m_curTab + 1); // bar index 0 is welcome
        return;
    }
    updateTabTitle(m_curTab);
    saveTabsNow();
    doBrowseSearch(false);
}

