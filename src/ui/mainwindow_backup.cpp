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

