#include "mainwindow.h"
#include "appinfo.h"
#include "cache.h"
#include "cachejanitor.h"
#include "cachepolicy.h"
#include "cookiegrab.h"
#include "csvbackup.h"
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
#include <QRegularExpression>
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

void MainWindow::launchImportDialog(LosnImportDialog *dlg) {
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    connect(dlg, &LosnImportDialog::wallSolveRequested, this,
            [this](const QString &siteKey, const QUrl &url) {
                solveWallInBrowser(siteKey, m_wallKinds.value(siteKey), url);
            });
    // Auto-refresh on close: importing touches the favourites DB and
    // the download folder, so the grid/fav tabs must not stay stale.
    connect(dlg, &QDialog::finished, this, [this] {
        refreshFavTabs();
        invalidateDownloadIndex();
        repaintFavMarks();
    });
    dlg->show();
    dlg->raise();
}

void MainWindow::importLosnBackup() {
    const QString path = QFileDialog::getOpenFileName(
        this, tr("LoliSnatcher backup"), QDir::homePath(),
        tr("SQLite database (*.db);;All files (*)"));
    if (path.isEmpty())
        return;
    launchImportDialog(new LosnImportDialog(
        path, &m_favDb, m_downloads, m_settings,
        effectiveDownloadDir(m_settings), this));
}

namespace {
// Local files carry no md5 field; the downloader puts it in the stem
// (<Site>_<md5>.jpg or <md5>.png, legacy imports: <Site>_losn<id>).
QString md5FromFileName(const QString &fileName) {
    static const QRegularExpression re(
        QStringLiteral("(?:^|_)([0-9a-f]{32})$"),
        QRegularExpression::CaseInsensitiveOption);
    const QString stem = QFileInfo(fileName).completeBaseName();
    const auto m = re.match(stem);
    return m.hasMatch() ? m.captured(1).toLower() : QString();
}

QString isoFromMsecs(qint64 msecs) {
    if (msecs <= 0)
        return {};
    return QDateTime::fromMSecsSinceEpoch(msecs)
        .toUTC()
        .toString(Qt::ISODate);
}

// A downloaded sidecar (gallery-dl `<media>.json` or `<stem>.json`)
// still knows the file URL even though the scanner does not keep it. It
// lets a CSV restore re-download without the API.
QString fileUrlFromSidecar(const QString &mediaPath) {
    const QFileInfo fi(mediaPath);
    const QStringList candidates = {
        fi.absoluteFilePath() + QStringLiteral(".json"),
        fi.absolutePath() + u'/' + fi.completeBaseName() +
            QStringLiteral(".json"),
    };
    for (const QString &path : candidates) {
        QFile f(path);
        if (!f.exists() || f.size() > 8LL * 1024 * 1024 ||
            !f.open(QIODevice::ReadOnly))
            continue;
        const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
        if (!doc.isObject())
            continue;
        const QJsonObject o = doc.object();
        for (const char *key :
             {"file_url", "fileUrl", "fileURL", "contentUrl", "media_url"}) {
            const QString url =
                o.value(QLatin1String(key)).toString().trimmed();
            if (RemotePosts::isHttpUrl(url))
                return url;
        }
    }
    return {};
}
} // namespace

void MainWindow::exportLibraryCsv() {
    const QString path = QFileDialog::getSaveFileName(
        this, tr("Export library CSV"),
        QDir::homePath() + QStringLiteral("/box-library.csv"),
        tr("CSV files (*.csv);;All files (*)"));
    if (path.isEmpty())
        return;

    QVector<CsvBackup::Row> rows;
    QHash<QString, int> seen; // identity -> row, so one post = one line
    const QString root =
        QDir(effectiveDownloadDir(m_settings)).absolutePath();

    auto siteLabelFor = [](const Post &p) -> QString {
        if (const BooruSite *e = RemotePosts::entryForPost(p))
            return e->label;
        if (!p.siteName.isEmpty())
            return p.siteName;
        return p.category;
    };
    auto postUrlFor = [](const Post &p) -> QString {
        if (!p.postUrl.isEmpty())
            return p.postUrl;
        if (p.id > 0) {
            if (const BooruSite *e = RemotePosts::entryForPost(p))
                return RemotePosts::postPageUrlFor(*e, p.id);
        }
        return {};
    };
    auto localFileFor = [&root](const Post &p) {
        if (p.absFile.isEmpty() || !QFileInfo::exists(p.absFile))
            return QString();
        const QString rel = QDir(root).relativeFilePath(p.absFile);
        return rel.startsWith(QStringLiteral("..")) ? p.absFile : rel;
    };
    auto identityKeys = [&](const Post &p) {
        QStringList keys;
        if (!p.absFile.isEmpty())
            keys << QStringLiteral("path:") +
                        QFileInfo(p.absFile).absoluteFilePath();
        const BooruSite *e = RemotePosts::entryForPost(p);
        const QString site = e ? e->key : p.siteName.toLower();
        if (!site.isEmpty() && p.id > 0)
            keys << QStringLiteral("id:") + site + u'#' +
                        QString::number(p.id);
        const QString post = LosnImport::diskUrlKey(p.postUrl, true);
        if (!post.isEmpty())
            keys << QStringLiteral("post:") + post;
        // Same file reached through a favourite row and the disk scan.
        QString md5 = p.md5.trimmed().toLower();
        if (md5.isEmpty() && !p.fileUrl.isEmpty())
            md5 = LosnImport::md5FromFileUrl(p.fileUrl);
        if (md5.isEmpty() && !p.absFile.isEmpty())
            md5 = md5FromFileName(p.absFile);
        if (!md5.isEmpty())
            keys << QStringLiteral("md5:") + md5;
        return keys;
    };
    auto addKeys = [&seen](int row, const QStringList &keys) {
        for (const QString &k : keys)
            if (!k.isEmpty() && !seen.contains(k))
                seen.insert(k, row);
    };

    // 1) Favourites: the authoritative site label + original add time.
    if (m_favDb.isOpen()) {
        const auto added = m_favDb.addedTimes();
        for (const Post &p : m_favDb.list(-1)) {
            CsvBackup::Row r;
            r.postUrl = postUrlFor(p);
            r.fileUrl = p.fileUrl;
            r.favourited = true;
            r.downloaded = isPostDownloaded(p);
            r.site = siteLabelFor(p);
            r.postId = p.id;
            r.md5 = p.md5.isEmpty() ? LosnImport::md5FromFileUrl(p.fileUrl)
                                    : p.md5.toLower();
            r.rating = p.rating;
            r.ext = p.ext;
            r.width = p.width;
            r.height = p.height;
            r.tags = p.tags;
            r.localFile = localFileFor(p);
            r.dateAdded = isoFromMsecs(
                added.value({FavDb::siteFor(p), FavDb::pidFor(p)}, 0));
            rows.append(r);
            addKeys(static_cast<int>(rows.size()) - 1, identityKeys(p));
        }
    }

    // 2) Files in the download folder that no favourite represented:
    // downloaded-only posts (and stray files with no metadata at all).
    if (QDir(root).exists()) {
        const QVector<Post> files =
            Scanner::scan(root, nullptr, m_settings.indexCache);
        for (const Post &p : files) {
            const QStringList keys = identityKeys(p);
            bool known = false;
            for (const QString &k : keys)
                if (seen.contains(k)) {
                    known = true;
                    break;
                }
            if (known)
                continue;
            CsvBackup::Row r;
            r.postUrl = postUrlFor(p);
            r.fileUrl = fileUrlFromSidecar(p.absFile);
            r.downloaded = true;
            r.site = siteLabelFor(p);
            r.postId = p.id;
            r.md5 = md5FromFileName(p.absFile);
            r.rating = p.rating;
            r.ext = p.ext;
            r.width = p.width;
            r.height = p.height;
            r.tags = p.tags;
            r.localFile = localFileFor(p);
            const QFileInfo fi(p.absFile);
            QDateTime stamp = fi.birthTime();
            if (!stamp.isValid())
                stamp = fi.lastModified();
            r.dateAdded = stamp.isValid()
                              ? stamp.toUTC().toString(Qt::ISODate)
                              : QString();
            rows.append(r);
            addKeys(static_cast<int>(rows.size()) - 1, keys);
        }
    }

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QMessageBox::warning(this, tr("Export library CSV"),
                             tr("Cannot write %1").arg(path));
        return;
    }
    f.write(CsvBackup::write(rows));
    f.close();
    statusBar()->showMessage(
        tr("Exported %1 entries to %2").arg(rows.size()).arg(path), 8000);
}

void MainWindow::importLibraryCsv() {
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Import library CSV"), QDir::homePath(),
        tr("CSV files (*.csv);;All files (*)"));
    if (path.isEmpty())
        return;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, tr("Import library CSV"),
                             tr("Cannot read %1").arg(path));
        return;
    }
    QString error;
    const QVector<CsvBackup::Row> rows =
        CsvBackup::parse(f.readAll(), &error);
    if (!error.isEmpty()) {
        QMessageBox::warning(
            this, tr("Import library CSV"),
            tr("%1 is not a usable backup:\n%2").arg(path, error));
        return;
    }
    if (rows.isEmpty()) {
        statusBar()->showMessage(tr("Backup has no entries."), 6000);
        return;
    }
    launchImportDialog(new LosnImportDialog(
        LosnImport::rowsFromCsv(rows), tr("Import CSV backup"), &m_favDb,
        m_downloads, m_settings, effectiveDownloadDir(m_settings), this));
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

