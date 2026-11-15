#include "losnimport.h"
#include "downloadmanager.h"
#include "favdb.h"
#include "progressbar.h"
#include "remotesource.h"
#include "settings.h"

#include <QCheckBox>
#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUrl>
#include <QUuid>
#include <QVBoxLayout>

namespace LosnImport {

void siteForHost(const QString &hostIn, QString &key, QString &label) {
    const QString host = hostIn.toLower();
    if (host.contains(QStringLiteral("danbooru"))) {
        key = QStringLiteral("danbooru");
        label = QStringLiteral("Danbooru");
    } else if (host.contains(QStringLiteral("donmai"))) {
        // CDN hosts (cdn.donmai.us) serve Danbooru files; merge-tab rows
        // often lack a post URL but keep these file URLs.
        key = QStringLiteral("danbooru");
        label = QStringLiteral("Danbooru");
    } else if (host.contains(QStringLiteral("gelbooru"))) {
        key = QStringLiteral("gelbooru");
        label = QStringLiteral("Gelbooru");
    } else if (host.contains(QStringLiteral("safebooru"))) {
        key = QStringLiteral("safebooru");
        label = QStringLiteral("Safebooru");
    } else if (host.contains(QStringLiteral("yande.re"))) {
        key = QStringLiteral("yandere");
        label = QStringLiteral("Yandere");
    } else if (host.contains(QStringLiteral("konachan"))) {
        key = QStringLiteral("konachan");
        label = QStringLiteral("Konachan");
    } else if (host.contains(QStringLiteral("aibooru"))) {
        key = QStringLiteral("aibooru");
        label = QStringLiteral("AIBooru");
    } else {
        key = {};
        // "rule34.xxx" -> "Rule34", "foo.bar.baz" -> "Baz".
        const QStringList parts = host.split(u'.', Qt::SkipEmptyParts);
        QString root = parts.size() >= 2 ? parts[parts.size() - 2] : host;
        if (!root.isEmpty())
            root[0] = root[0].toUpper();
        label = root.isEmpty() ? QStringLiteral("LoliSnatcher") : root;
    }
}

qint64 idFromPostUrl(const QString &postUrl) {
    // ...id=12345  |  /posts/12345  |  /post/show/12345  |  /post/view/12345
    static const QRegularExpression re(
        QStringLiteral("(?:[?&]id=|/posts?/(?:show/|view/)?)(\\d+)"));
    const auto m = re.match(postUrl);
    return m.hasMatch() ? m.captured(1).toLongLong() : 0;
}

QString md5FromFileUrl(const QString &fileUrl) {
    const QString stem = QUrl(fileUrl).fileName().split(u'.').first();
    static const QRegularExpression hex32(QStringLiteral("^[0-9a-f]{32}$"));
    return hex32.match(stem).hasMatch() ? stem : QString();
}

QHash<QString, QString> md5PathsOnDisk(const QString &destDir) {
    // One listing for the whole tree (not per row): file stems carry the
    // content hash, so any earlier filing (even under a stale "merge"
    // label or a host folder used by the regular downloader) counts as
    // already imported. Case-insensitive: API/URL hashes differ in case.
    static const QRegularExpression md5Stem(
        QStringLiteral("_(?P<md5>[0-9a-f]{32})\\.[^.]+$"),
        QRegularExpression::CaseInsensitiveOption);
    QHash<QString, QString> out;
    const QDir dest(destDir);
    if (destDir.isEmpty() || !dest.exists())
        return out;
    // Recursive: the download tree is normally one site level, but a user
    // reorganising files into nested folders must not cause re-downloads.
    QDirIterator it(destDir, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QFileInfo fi(it.next());
        const auto m = md5Stem.match(fi.fileName());
        if (!m.hasMatch())
            continue;
        const QString md5 = m.captured(QStringLiteral("md5")).toLower();
        if (!out.contains(md5))
            out.insert(md5, fi.absoluteFilePath());
    }
    return out;
}

QSet<QString> md5sOnDisk(const QString &destDir) {
    const QHash<QString, QString> paths = md5PathsOnDisk(destDir);
    QSet<QString> out;
    out.reserve(paths.size());
    for (auto it = paths.begin(); it != paths.end(); ++it)
        out.insert(it.key());
    return out;
}

SummaryDiff diffNewFiles(const QSet<QString> &beforeMd5,
                         const QList<QPair<QString, QString>> &finalPaths) {
    // Same `_<md5>.<ext>` stem rule as md5sOnDisk; md5-less names (the
    // manager hashes those, so finalPaths normally carries md5s) count new.
    static const QRegularExpression md5Stem(
        QStringLiteral("_(?P<md5>[0-9a-f]{32})\\.[^.]+$"));
    SummaryDiff out;
    for (const auto &[url, path] : finalPaths) {
        (void)url;
        const QString name = QFileInfo(path).fileName();
        if (name.isEmpty())
            continue;
        const auto m = md5Stem.match(name);
        const QString md5 =
            m.hasMatch() ? m.captured(QStringLiteral("md5")) : QString();
        if (!md5.isEmpty() && beforeMd5.contains(md5))
            out.alreadyPresent.append(name);
        else
            out.newFiles.append(name);
    }
    QSet<QString> seen;
    out.newFiles.erase(
        std::remove_if(out.newFiles.begin(), out.newFiles.end(),
                       [&seen](const QString &n) {
                           const bool dup = seen.contains(n);
                           seen.insert(n);
                           return dup;
                       }),
        out.newFiles.end());
    seen.clear();
    out.alreadyPresent.erase(
        std::remove_if(out.alreadyPresent.begin(), out.alreadyPresent.end(),
                       [&seen](const QString &n) {
                           const bool dup = seen.contains(n);
                           seen.insert(n);
                           return dup;
                       }),
        out.alreadyPresent.end());
    return out;
}

DownloadPaths pathsForRow(const LosnRow &row, const QJsonObject &sidecar,
                          const QString &destDir) {
    LosnRow named = row;
    QString smd = sidecar.value(QStringLiteral("md5")).toString();
    if (smd.isEmpty())
        smd = sidecar.value(QStringLiteral("checksumMD5")).toString();
    // Hex only: a rogue sidecar md5 ("../../x") must never become a
    // filename (path traversal into the download folder and beyond).
    // Anything else falls back to the _losn<rowId> name below.
    smd = smd.trimmed().toLower();
    static const QRegularExpression hex32(QStringLiteral("^[0-9a-f]{32}$"));
    if (!smd.isEmpty() && hex32.match(smd).hasMatch())
        named.md5 = smd;
    return targetPaths(named, destDir);
}

namespace {
// Folder tag the regular downloader would use for this backup row: the
// configured entry's base-URL host, else the row's own (post, then file)
// host so unconfigured sources still match host-named folders instead of
// the legacy display label.
QString folderTagForRow(const LosnRow &row) {
    Post p;
    p.isRemote = true;
    p.siteKey = row.siteKey;
    p.siteName = row.siteLabel;
    p.fileUrl = row.fileUrl;
    p.postUrl = row.postUrl;
    const QString tag = RemotePosts::fsTagForPost(p);
    if (tag == row.siteLabel && !row.host.isEmpty())
        return row.host;
    return tag;
}
} // namespace

DownloadPaths targetPaths(const LosnRow &row, const QString &destDir) {
    // Same folder + filename shape as DownloadManager::plan(), so an
    // import lands beside the regular downloader's files (gelbooru.com/,
    // gelboorucom_<md5>.jpg) instead of a parallel <Label>/ tree.
    DownloadPaths out;
    QString md5 = row.md5.trimmed().toLower();
    static const QRegularExpression hex32(QStringLiteral("^[0-9a-f]{32}$"));
    if (!hex32.match(md5).hasMatch())
        md5.clear();
    const QString ext =
        BooruEngineOps::extOf(row.fileUrl, QStringLiteral("jpg"));
    QJsonObject raw;
    raw[QStringLiteral("file_ext")] = ext;
    if (!md5.isEmpty())
        raw[QStringLiteral("md5")] = md5;
    const QString tag = folderTagForRow(row);
    QString stem = RemotePosts::downloadFileName(tag, raw);
    if (md5.isEmpty()) {
        // Stable fallback stem (re-imports agree on the path) that can
        // never be mistaken for a real content hash.
        const int dot = stem.lastIndexOf(u'.');
        const QString extPart = dot > 0 ? stem.mid(dot) : QString();
        const QString base = dot > 0 ? stem.left(dot) : stem;
        const int cut = base.lastIndexOf(u'_');
        const QString prefix = cut > 0 ? base.left(cut) : base;
        stem = prefix + QStringLiteral("_losn") + QString::number(row.rowId) +
               extPart;
    }
    const QString dir = destDir + u'/' + RemotePosts::safeFolderSegment(tag);
    out.fileName = stem;
    out.filePath = dir + u'/' + out.fileName;
    out.jsonPath =
        dir + u'/' + stem.left(stem.lastIndexOf(u'.')) + QStringLiteral(".json");
    return out;
}

QJsonObject nativeSidecar(const LosnRow &row) {
    // Gallery-dl danbooru-native shape (superset: our scanner routes it).
    QJsonObject o;
    o[QStringLiteral("id")] = row.postId;
    o[QStringLiteral("score")] = 0;
    o[QStringLiteral("rating")] = QStringLiteral("g");
    o[QStringLiteral("tag_string_artist")] = QString();
    o[QStringLiteral("tag_string_character")] = QString();
    o[QStringLiteral("tag_string_copyright")] = QString();
    o[QStringLiteral("tag_string_general")] = row.tags.join(u' ');
    o[QStringLiteral("tag_string_meta")] = QString();
    o[QStringLiteral("tag_string")] = row.tags.join(u' ');
    const QString ext = QFileInfo(QUrl(row.fileUrl).path()).suffix().toLower();
    o[QStringLiteral("file_ext")] = ext;
    o[QStringLiteral("md5")] = row.md5;
    o[QStringLiteral("source")] = QString();
    o[QStringLiteral("created_at")] = QString();
    o[QStringLiteral("file_url")] = row.fileUrl;
    return o;
}

QList<LosnRow> readBackup(const QString &dbPath, QString &error) {
    QList<LosnRow> rows;
    const QString conn = QStringLiteral("losn_%1").arg(
        QUuid::createUuid().toString(QUuid::WithoutBraces));
    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
    db.setDatabaseName(dbPath);
    db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY=1"));
    if (!db.open()) {
        error = db.lastError().text();
        QSqlDatabase::removeDatabase(conn);
        return rows;
    }
    // tag id -> name
    // Tables only, capped and forward-only: a hostile file can alias
    // these names to cartesian VIEWs that explode a KB file into GBs of
    // logical rows. Real backups (50 MB+) sit far below the caps.
    auto tableOk = [&](const QString &name) {
        QSqlQuery q(db);
        q.prepare(QStringLiteral(
            "SELECT type FROM sqlite_master WHERE name=?"));
        q.bindValue(0, name);
        return q.exec() && q.next() &&
               q.value(0).toString() == QStringLiteral("table");
    };
    if (!tableOk(QStringLiteral("BooruItem"))) {
        error = QCoreApplication::translate(
            "LosnImportDialog",
            "Not a LoliSnatcher backup (BooruItem table missing)");
        db.close();
        QSqlDatabase::removeDatabase(conn);
        return rows;
    }
    static constexpr int kMaxTags = 1000000;
    static constexpr int kMaxItemTags = 4000000;
    static constexpr int kMaxItems = 1000000;
    QHash<qint64, QString> tagNames;
    if (tableOk(QStringLiteral("Tag"))) {
        QSqlQuery q(db);
        q.setForwardOnly(true);
        if (q.exec(QStringLiteral("SELECT id, name FROM Tag LIMIT ") + QString::number(kMaxTags))) {
            while (q.next())
                tagNames.insert(q.value(0).toLongLong(), q.value(1).toString());
        }
    }
    // item id -> tags
    QHash<qint64, QStringList> itemTags;
    if (tableOk(QStringLiteral("ImageTag"))) {
        QSqlQuery q(db);
        q.setForwardOnly(true);
        if (q.exec(QStringLiteral("SELECT tagID, booruItemID FROM ImageTag LIMIT ") + QString::number(kMaxItemTags))) {
            while (q.next()) {
                const QString name = tagNames.value(q.value(0).toLongLong());
                if (!name.isEmpty())
                    itemTags[q.value(1).toLongLong()].append(name);
            }
        }
    }
    {
        QSqlQuery q(db);
        q.setForwardOnly(true);
        if (!q.exec(QStringLiteral("SELECT id, thumbnailURL, sampleURL, fileURL, "
                                   "postURL, mediaType, isSnatched, isFavourite "
                                   "FROM BooruItem LIMIT ") + QString::number(kMaxItems))) {
            error = q.lastError().text();
            db.close();
            QSqlDatabase::removeDatabase(conn);
            return rows;
        }
        while (q.next()) {
            LosnRow r;
            r.rowId = q.value(0).toLongLong();
            r.thumbUrl = q.value(1).toString();
            r.sampleUrl = q.value(2).toString();
            r.fileUrl = q.value(3).toString();
            r.postUrl = q.value(4).toString();
            r.mediaType = q.value(5).toString();
            r.snatched = q.value(6).toLongLong() != 0;
            r.favourite = q.value(7).toLongLong() != 0;
            r.tags = itemTags.value(r.rowId);
            r.host = QUrl(r.postUrl).host().toLower();
            siteForHost(r.host, r.siteKey, r.siteLabel);
            if (r.siteKey.isEmpty() && !r.fileUrl.isEmpty()) {
                // Merge-tab rows often lack a post URL: fall back to the
                // file (CDN) host so they file under the real booru instead
                // of a "merge"/"LoliSnatcher" catch-all.
                const QString fileHost = QUrl(r.fileUrl).host().toLower();
                siteForHost(fileHost, r.siteKey, r.siteLabel);
                if (r.host.isEmpty())
                    r.host = fileHost;
            }
            r.postId = idFromPostUrl(r.postUrl);
            r.md5 = md5FromFileUrl(r.fileUrl);
            rows.append(r);
        }
    }
    db.close();
    QSqlDatabase::removeDatabase(conn);
    return rows;
}

} // namespace LosnImport

// ── dialog ─────────────────────────────────────────────────────

LosnImportDialog::LosnImportDialog(const QString &dbPath, FavDb *favDb,
                                   DownloadManager *dl, const AppSettings &settings,
                                   const QString &downloadDir, QWidget *parent)
    : QDialog(parent), m_dbPath(dbPath), m_downloadDir(downloadDir),
      m_favDb(favDb), m_downloads(dl), m_settings(settings) {
    for (int i = 0; i < kMetaFlights; ++i) {
        auto *flight = new PostRefresher(this);
        connect(flight, &PostRefresher::refreshed, this,
                &LosnImportDialog::onMetaDone);
        connect(flight, &PostRefresher::failed, this,
                &LosnImportDialog::onMetaFailed);
        m_metaFlights.append(flight);
    }
    setWindowTitle(tr("Import LoliSnatcher backup"));
    resize(560, 480);
    auto *lay = new QVBoxLayout(this);

    QString error;
    m_rows = LosnImport::readBackup(dbPath, error);
    for (int i = 0; i < m_rows.size(); ++i)
        m_rowIndex.insert(m_rows.at(i).fileUrl, i);
    for (const LosnRow &r : m_rows) {
        if (r.favourite)
            m_favCount++;
        if (r.snatched)
            m_snatchCount++;
    }
    if (!error.isEmpty() && m_rows.isEmpty()) {
        lay->addWidget(new QLabel(tr("Cannot read backup: %1").arg(error), this));
        auto *closeBtn = new QPushButton(tr("Close"), this);
        connect(closeBtn, &QPushButton::clicked, this, &QDialog::reject);
        lay->addWidget(closeBtn);
        return;
    }

    m_counts = new QLabel(
        tr("%1 favourites, %2 snatched (of %3 entries)")
            .arg(m_favCount)
            .arg(m_snatchCount)
            .arg(m_rows.size()),
        this);
    lay->addWidget(m_counts);
    m_target = new QLabel(
        tr("Target: download folder %1").arg(m_downloadDir),
        this);
    m_target->setStyleSheet(QStringLiteral("color:#4a5068;"));
    m_target->setWordWrap(true);
    lay->addWidget(m_target);

    auto *favsBox = new QGroupBox(tr("Favourited entries (♥)"), this);
    auto *favsLay = new QVBoxLayout(favsBox);
    m_importFavs = new QCheckBox(tr("Import to Favourites"), this);
    m_importFavs->setChecked(true);
    m_downloadFavs = new QCheckBox(
        tr("Also download them (and add the files to the download folder)"),
        this);
    m_downloadFavs->setChecked(false);
    favsLay->addWidget(m_importFavs);
    favsLay->addWidget(m_downloadFavs);
    m_sync = new QCheckBox(
        tr("Sync: remove local favourites/downloads not in this backup"),
        this);
    m_sync->setChecked(false);
    favsLay->addWidget(m_sync);
    lay->addWidget(favsBox);

    auto *opts = new QGroupBox(tr("Downloaded entries (↓)"), this);
    auto *optsLay = new QVBoxLayout(opts);
    m_snatchedDownload =
        new QRadioButton(tr("Download files and keep the ↓ mark"), this);
    m_snatchedFav = new QRadioButton(
        tr("Import as favourites only (no files; use after Android deleted "
           "the originals)"),
        this);
    m_snatchedSkip = new QRadioButton(tr("Skip these entries"), this);
    m_snatchedDownload->setChecked(true);
    m_favDownloaded = new QCheckBox(
        tr("Also add the downloaded entries to Favourites"), this);
    m_favDownloaded->setChecked(false);
    optsLay->addWidget(m_snatchedDownload);
    optsLay->addWidget(m_snatchedFav);
    optsLay->addWidget(m_snatchedSkip);
    optsLay->addWidget(m_favDownloaded);
    lay->addWidget(opts);
    auto syncFavDownloaded = [this] {
        m_favDownloaded->setEnabled(m_snatchedDownload->isChecked());
    };
    connect(m_snatchedDownload, &QRadioButton::toggled, this,
            [syncFavDownloaded](bool) { syncFavDownloaded(); });
    syncFavDownloaded();

    lay->addWidget(new QLabel(
        tr("Downloads save full quality + gallery-dl-style sidecars."), this));
    m_bar = new CenterProgressBar(this);
    m_bar->setVisible(false);
    lay->addWidget(m_bar);
    m_log = new QListWidget(this);
    lay->addWidget(m_log, 1);

    auto *row = new QHBoxLayout();
    m_startBtn = new QPushButton(tr("Start import"), this);
    connect(m_startBtn, &QPushButton::clicked, this, &LosnImportDialog::onStart);
    auto *closeBtn = new QPushButton(tr("Close"), this);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::reject);
    row->addStretch(1);
    row->addWidget(m_startBtn);
    row->addWidget(closeBtn);
    lay->addLayout(row);

    connect(m_downloads, &DownloadManager::finished, this,
            &LosnImportDialog::onDownloadsDone);
    connect(m_downloads, &DownloadManager::fileProgress, this,
            &LosnImportDialog::onFileProgress);
    connect(m_downloads, &DownloadManager::fileRenamed, this,
            &LosnImportDialog::onFileRenamed);
    // Progress routing is connected once here (not per run): concurrent
    // metadata results share one bar, downloads reuse it afterwards.
    connect(m_downloads, &DownloadManager::progress, this,
            [this](int done, int total, const QString &) {
                if (!m_expectingDownloads)
                    return;
                m_bar->setValue(m_refreshTotal + m_metaTotal + done);
                m_bar->setMaximum(m_refreshTotal + m_metaTotal +
                                  qMax(1, total));
            });
}

void LosnImportDialog::log(const QString &text) {
    m_log->addItem(text);
    m_log->scrollToBottom();
}

void LosnImportDialog::setBusy(bool busy) {
    m_startBtn->setEnabled(!busy);
    m_bar->setVisible(busy);
}

static RefreshTarget targetForRow(const LosnRow &row, const AppSettings &s) {
    RefreshTarget t;
    if (row.siteKey.isEmpty() || row.postId <= 0)
        return t;
    const BooruSite *site = nullptr;
    for (const BooruSite &b : s.boorus) {
        if (b.key == row.siteKey) {
            site = &b;
            break;
        }
    }
    if (!site)
        site = RemoteSource::siteFor(row.siteKey); // presets (aibooru etc.)
    if (!site || site->baseUrl.isEmpty())
        return t;
    t.siteKey = site->key;
    t.baseUrl = site->baseUrl;
    t.id = row.postId;
    t.creds.user = site->user;
    t.creds.secret = site->secret;
    return t;
}

void LosnImportDialog::onStart() {
    if (!m_favDb || !m_favDb->isOpen()) {
        log(tr("Favourites database unavailable."));
        return;
    }
    if (m_downloads->isActive()) {
        log(tr("Finish the current downloads first."));
        return;
    }
    m_downloadRows.clear();
    // Pre-import disk snapshot: the finish summary diffs finalPaths
    // against these md5 stems to list only genuinely new files.
    m_diskByMd5 = LosnImport::md5PathsOnDisk(m_downloadDir);
    m_diskPaths.clear();
    m_beforeMd5.clear();
    for (auto it = m_diskByMd5.begin(); it != m_diskByMd5.end(); ++it)
        m_beforeMd5.insert(it.key());
    auto rowKey = [](const LosnRow &r) {
        if (r.postId > 0)
            return r.siteLabel + u'#' + QString::number(r.postId);
        return r.fileUrl.isEmpty() ? QStringLiteral("row#") +
                                         QString::number(r.rowId)
                                   : r.fileUrl;
    };
    // Selection matrix: favourite rows and downloaded rows are separate
    // LOSN flags, and Android often leaves the ↓ flag behind after the
    // file was deleted. Each side can be imported, crossed, or skipped.
    QList<LosnRow> favRows;
    if (m_importFavs->isChecked()) {
        for (const LosnRow &r : m_rows)
            if (r.favourite)
                favRows.append(r);
    }
    const bool favSnatched = m_snatchedFav->isChecked();
    const bool downloadSnatched = m_snatchedDownload->isChecked();
    // Phase 1: favourites (DB only, one transaction).
    QList<LosnRow> toFavourite;
    QSet<QString> favSeen;
    auto addFavourite = [&](const LosnRow &r) {
        const QString key = rowKey(r);
        if (favSeen.contains(key))
            return;
        favSeen.insert(key);
        toFavourite.append(r);
    };
    for (const LosnRow &r : favRows)
        addFavourite(r);
    if (favSnatched) {
        for (const LosnRow &r : m_rows)
            if (r.snatched)
                addFavourite(r);
    } else if (downloadSnatched && m_favDownloaded->isChecked()) {
        for (const LosnRow &r : m_rows)
            if (r.snatched)
                addFavourite(r);
    }
    if (!toFavourite.isEmpty()) {
        QVector<Post> favs;
        QHash<QPair<QString, qint64>, QString> paths;
        for (const LosnRow &r : toFavourite) {
            Post p;
            p.isRemote = true;
            p.siteName = r.siteLabel;
            p.id = r.postId;
            p.tags = r.tags.join(u' ');
            p.tagsGeneral = p.tags;
            p.untyped = p.tags; // backup tags are category-less by nature
            p.fileUrl = r.fileUrl;
            p.sampleUrl = r.sampleUrl;
            p.previewUrl = r.thumbUrl;
            p.postUrl = r.postUrl;
            p.hasMeta = true;
            favs.append(p);
            // Link only files that actually exist: a target path for a
            // never-downloaded row would later look like a deleted
            // download (redownload prompt / stale ↓ flag).
            QString target = LosnImport::targetPaths(r, m_downloadDir).filePath;
            if (!QFileInfo::exists(target) && !r.md5.isEmpty())
                target = m_diskByMd5.value(r.md5.toLower());
            if (!target.isEmpty() && QFileInfo::exists(target))
                paths.insert({r.siteLabel, r.postId}, target);
        }
        const int n = m_favDb->addMany(favs);
        if (!paths.isEmpty())
            m_favDb->setLocalPaths(paths);
        log(tr("Imported %1 favourite entries.").arg(n));
    }
    if (m_sync && m_sync->isChecked()) {
        QSet<QString> imported;
        for (const LosnRow &r : toFavourite)
            imported.insert(r.siteLabel + u'#' + QString::number(r.postId));
        int removed = 0;
        for (const Post &p : m_favDb->list()) {
            const QString key = p.siteName + u'#' + QString::number(p.id);
            if (imported.contains(key))
                continue;
            if (m_favDb->remove(p.siteName, p.id))
                removed++;
        }
        if (removed > 0)
            log(tr("Sync removed %1 stale favourite entries.").arg(removed));
    }
    // Phase 2/3: downloads (snatched rows, optionally favourites too).
    m_downloadRows.clear();
    QSet<QString> dlSeen;
    auto addDownload = [&](const LosnRow &r) {
        if (r.fileUrl.isEmpty())
            return;
        const QString key = rowKey(r);
        if (dlSeen.contains(key))
            return;
        dlSeen.insert(key);
        m_downloadRows.append(r);
    };
    if (downloadSnatched) {
        for (const LosnRow &r : m_rows)
            if (r.snatched)
                addDownload(r);
    }
    if (m_downloadFavs->isChecked()) {
        for (const LosnRow &r : m_rows)
            if (r.favourite)
                addDownload(r);
    }
    if (m_downloadRows.isEmpty()) {
        log(tr("Nothing to download."));
        log(tr("Done."));
        return;
    }
    setBusy(true);
    // Re-imports only fetch what's new for MEDIA: rows already on disk
    // keep their files, but their sidecars are re-fetched and rewritten
    // below (never trusted from an earlier run).
    m_metaPending.clear();
    m_refreshPending.clear();
    m_refreshUrls.clear();
    for (const LosnRow &r : m_downloadRows) {
        const QString target =
            LosnImport::targetPaths(r, m_downloadDir).filePath;
        QString onDisk =
            QFile::exists(target)
                ? target
                : m_diskByMd5.value(r.md5.trimmed().toLower());
        if (!onDisk.isEmpty() && onDisk != target && !QFile::exists(target)) {
            // Upgrade an older import that filed the file under <Label>/
            // (and <Label>_<md5>) into the regular downloader's host
            // folder, so the tree stays consistent. Only app-made legacy
            // label folders are touched; user-relocated files stay put.
            const QString legacyDir =
                m_downloadDir + u'/' + RemotePosts::safeFolderSegment(
                                              r.siteLabel);
            if (QFileInfo(onDisk).absolutePath() == legacyDir) {
                QDir().mkpath(QFileInfo(target).absolutePath());
                if (QFile::rename(onDisk, target)) {
                    const QString oldStem =
                        QFileInfo(onDisk).completeBaseName();
                    const QString newStem =
                        QFileInfo(target).completeBaseName();
                    const QString oldJson = QFileInfo(onDisk).absolutePath() +
                                            u'/' + oldStem +
                                            QStringLiteral(".json");
                    const QString newJson = QFileInfo(target).absolutePath() +
                                            u'/' + newStem +
                                            QStringLiteral(".json");
                    if (QFile::exists(oldJson) && !QFile::exists(newJson))
                        QFile::rename(oldJson, newJson);
                    onDisk = target;
                    m_diskByMd5.insert(r.md5.trimmed().toLower(), target);
                }
            }
        }
        if (!onDisk.isEmpty() && QFile::exists(onDisk)) {
            // Same content anywhere under the download folder (host
            // folder from the regular downloader, legacy label folder
            // from an earlier import): never fetch the media again.
            m_refreshPending.append(r);
            m_refreshUrls.insert(r.fileUrl);
            m_diskPaths.insert(r.fileUrl, onDisk);
        } else {
            m_metaPending.append(r);
        }
    }
    if (!m_refreshPending.isEmpty())
        log(tr("%1 already downloaded - refreshing sidecars.")
                .arg(m_refreshPending.size()));
    m_metaDone = 0;
    m_metaFailed = 0;
    m_metaTotal = m_metaPending.size();
    m_refreshDone = 0;
    m_refreshOk = 0;
    m_refreshFailed = 0;
    m_refreshTotal = m_refreshPending.size();
    m_sidecars.clear();
    m_flightUrl.clear();
    m_reqFlight.clear();
    // Rows with no refreshable origin can never re-fetch: the media still
    // downloads, but the sidecar is backup data - and said out loud.
    int unrefreshable = 0;
    for (int i = m_metaPending.size() - 1; i >= 0; --i) {
        const LosnRow &r = m_metaPending.at(i);
        if (!targetForRow(r, m_settings).valid()) {
            m_sidecars.insert(r.fileUrl, LosnImport::nativeSidecar(r));
            m_metaPending.removeAt(i);
            m_metaDone++;
            unrefreshable++;
        }
    }
    // Same for on-disk rows: no reachable source, sidecar stays as is.
    for (int i = m_refreshPending.size() - 1; i >= 0; --i) {
        const LosnRow &r = m_refreshPending.at(i);
        if (!targetForRow(r, m_settings).valid()) {
            m_refreshPending.removeAt(i);
            m_refreshUrls.remove(r.fileUrl);
            unrefreshable++;
        }
    }
    if (unrefreshable > 0)
        log(tr("%1 post(s) have no reachable source - backup data kept.")
                .arg(unrefreshable));
    if (m_metaPending.isEmpty() && m_refreshPending.isEmpty()) {
        setBusy(false);
        log(tr("Nothing new to download."));
        return;
    }
    // Metadata pass + download pass, preceded by the sidecar refresh.
    m_bar->setRange(0, m_refreshTotal + m_metaTotal * 2);
    m_bar->setValue(m_refreshDone + m_metaDone);
    if (!m_refreshPending.isEmpty())
        log(tr("Refreshing %1 sidecar(s) from the API…").arg(m_refreshTotal));
    if (!m_metaPending.isEmpty())
        log(tr("Fetching %1 post records for gallery-dl-style sidecars (%2 at a "
               "time)…")
                .arg(m_metaTotal)
                .arg(kMetaFlights));
    pumpMeta();
}

void LosnImportDialog::pumpMeta() {
    // Keep up to kMetaFlights fetches in flight; each completion pumps the
    // next row, so the queue drains ~4x faster and the UI never blocks.
    // Sidecar refreshes go first (they unblock nothing, but finish fast
    // and keep the log ordered: refresh, then fetch, then download).
    for (PostRefresher *flight : m_metaFlights) {
        bool refresh = false;
        LosnRow r;
        if (!m_refreshPending.isEmpty()) {
            r = m_refreshPending.takeFirst();
            refresh = true;
        } else if (!m_metaPending.isEmpty()) {
            r = m_metaPending.takeFirst();
        } else {
            break;
        }
        if (m_reqFlight.values().contains(flight)) {
            // Still fetching: never cancel a live flight. Put the row back
            // at the front so order is preserved.
            if (refresh)
                m_refreshPending.prepend(r);
            else
                m_metaPending.prepend(r);
            continue;
        }
        const RefreshTarget t = targetForRow(r, m_settings);
        if (!t.valid()) { // origin vanished mid-run (settings edited)
            if (refresh) {
                m_refreshUrls.remove(r.fileUrl);
                m_refreshDone++;
                m_refreshFailed++;
                m_bar->setValue(m_refreshDone + m_metaDone);
            } else {
                m_sidecars.insert(r.fileUrl, LosnImport::nativeSidecar(r));
                m_metaDone++;
                m_bar->setValue(m_refreshDone + m_metaDone);
            }
            continue;
        }
        const qint64 id = m_nextReqId++;
        m_flightUrl.insert(id, r.fileUrl);
        m_reqFlight.insert(id, flight);
        flight->refresh(t, id);
    }
    if (m_metaDone >= m_metaTotal && m_flightUrl.isEmpty())
        startDownloads();
}

static void throttleLog(QListWidget *log, int done, int total) {
    // One row per completion makes huge imports crawl the item view.
    if (done == total || done % 25 == 0)
        log->addItem(QStringLiteral("%1 / %2").arg(done).arg(total));
}

void LosnImportDialog::onMetaDone(qint64 reqId, const QJsonObject &raw) {
    const QString url = m_flightUrl.take(reqId);
    m_reqFlight.remove(reqId);
    if (!url.isEmpty())
        m_sidecars.insert(url, raw);
    if (!url.isEmpty() && m_refreshUrls.contains(url)) {
        // Re-fetched object replaces whatever sidecar is on disk.
        if (m_rowIndex.contains(url)) {
            const LosnRow &r = m_rows.at(m_rowIndex.value(url));
            // Write beside the media that actually exists (it may live in
            // a host folder or an older label folder), never next to a
            // non-existent planned path.
            const QString media = m_diskPaths.value(url);
            const QString jsonPath =
                media.isEmpty()
                    ? LosnImport::targetPaths(r, m_downloadDir).jsonPath
                    : QFileInfo(media).absolutePath() + u'/' +
                          QFileInfo(media).completeBaseName() +
                          QStringLiteral(".json");
            QFile jf(jsonPath);
            if (jf.open(QIODevice::WriteOnly | QIODevice::Truncate))
                jf.write(QJsonDocument(raw).toJson(QJsonDocument::Indented));
            else
                log(tr("Cannot rewrite sidecar for %1 #%2.")
                        .arg(r.siteLabel)
                        .arg(r.postId));
        }
        m_refreshDone++;
        m_refreshOk++;
        m_bar->setValue(m_refreshDone + m_metaDone);
        throttleLog(m_log, m_refreshDone, m_refreshTotal);
        m_log->scrollToBottom();
        pumpMeta();
        return;
    }
    m_metaDone++;
    m_bar->setValue(m_refreshDone + m_metaDone);
    throttleLog(m_log, m_metaDone, m_metaTotal);
    m_log->scrollToBottom();
    pumpMeta();
}

void LosnImportDialog::onMetaFailed(qint64 reqId, const QString &) {
    // Fall back to the converted sidecar (offline / gone post / no key) -
    // except for refresh rows, where the sidecar on disk stays untouched.
    const QString url = m_flightUrl.take(reqId);
    m_reqFlight.remove(reqId);
    if (!url.isEmpty() && m_refreshUrls.contains(url)) {
        m_refreshDone++;
        m_refreshFailed++;
        m_bar->setValue(m_refreshDone + m_metaDone);
        if (m_rowIndex.contains(url) && m_refreshFailed <= 20) {
            const LosnRow &r = m_rows.at(m_rowIndex.value(url));
            log(tr("Sidecar refresh failed for %1 #%2 - kept existing.")
                    .arg(r.siteLabel)
                    .arg(r.postId));
        }
        throttleLog(m_log, m_refreshDone, m_refreshTotal);
        m_log->scrollToBottom();
        pumpMeta();
        return;
    }
    if (!url.isEmpty() && m_rowIndex.contains(url))
        m_sidecars.insert(url, LosnImport::nativeSidecar(m_rows.at(m_rowIndex.value(url))));
    m_metaDone++;
    m_metaFailed++;
    m_bar->setValue(m_refreshDone + m_metaDone);
    throttleLog(m_log, m_metaDone, m_metaTotal);
    m_log->scrollToBottom();
    pumpMeta();
}

void LosnImportDialog::startDownloads() {
    // Phase 3: enqueue real downloads.
    QList<DownloadJob> jobs;
    m_jobPaths.clear();
    const QString dest = m_downloadDir;
    // Re-imports skip what is already on disk ANYWHERE under dest (not
    // just at the computed path): earlier runs may have filed the same
    // md5 under a stale label (merge_* orphans), and the regular
    // downloader uses host folders.
    const QHash<QString, QString> haveMd5 =
        LosnImport::md5PathsOnDisk(dest);
    int skippedMd5 = 0;
    int skippedDuplicate = 0;
    QHash<QString, QString> batchMd5; // md5 -> first planned path
    QSet<QString> batchPaths;         // first planned path per file
    for (const LosnRow &r : m_downloadRows) {
        if (r.fileUrl.isEmpty())
            continue;
        QJsonObject sidecar = m_sidecars.value(r.fileUrl);
        if (sidecar.isEmpty())
            sidecar = LosnImport::nativeSidecar(r);
        const LosnImport::DownloadPaths paths =
            LosnImport::pathsForRow(r, sidecar, dest);
        if (QFile::exists(paths.filePath)) {
            m_diskPaths.insert(r.fileUrl, paths.filePath);
            continue; // re-import: already on disk
        }
        QString md5 = sidecar.value(QStringLiteral("md5")).toString();
        if (md5.isEmpty())
            md5 = sidecar.value(QStringLiteral("checksumMD5")).toString();
        if (md5.isEmpty())
            md5 = r.md5;
        md5 = md5.trimmed().toLower();
        if (!md5.isEmpty() && haveMd5.contains(md5)) {
            // Same bytes under another folder: keep them, but refresh the
            // sidecar beside the file that already exists.
            const QString media = haveMd5.value(md5);
            m_diskPaths.insert(r.fileUrl, media);
            const QFileInfo fi(media);
            QFile jf(fi.absolutePath() + u'/' + fi.completeBaseName() +
                     QStringLiteral(".json"));
            if (jf.open(QIODevice::WriteOnly | QIODevice::Truncate))
                jf.write(QJsonDocument(sidecar)
                             .toJson(QJsonDocument::Indented));
            skippedMd5++;
            continue; // same bytes elsewhere: nothing to fetch
        }
        // Duplicate rows inside this backup: same content hash, or two
        // rows resolving to one path. Queue a single fetch; both
        // favourites link to the one file afterwards.
        if (!md5.isEmpty() && batchMd5.contains(md5)) {
            m_diskPaths.insert(r.fileUrl, batchMd5.value(md5));
            skippedDuplicate++;
            continue;
        }
        if (batchPaths.contains(paths.filePath)) {
            m_diskPaths.insert(r.fileUrl, paths.filePath);
            skippedDuplicate++;
            continue;
        }
        if (!md5.isEmpty())
            batchMd5.insert(md5, paths.filePath);
        batchPaths.insert(paths.filePath);
        DownloadJob job;
        job.fileName = paths.fileName;
        job.filePath = paths.filePath;
        job.jsonPath = paths.jsonPath;
        Post p;
        p.isRemote = true;
        p.siteName = r.siteLabel;
        p.siteKey = r.siteKey; // folder/referer/cookies as the regular pull
        p.id = r.postId;
        p.fileUrl = r.fileUrl;
        p.md5 = md5;
        // Stash the sidecar object for the manager to write.
        p.rawJson = sidecar;
        job.post = p;
        m_jobPaths.insert(job.fileName, job.filePath);
        jobs.append(job);
    }
    setBusy(false);
    if (m_refreshTotal > 0)
        log(tr("Sidecars refreshed: %1 ok, %2 failed (kept existing).")
                .arg(m_refreshOk)
                .arg(m_refreshFailed));
    if (m_metaFailed > 0)
        log(tr("%1 download(s) use backup sidecars (fetch failed).")
                .arg(m_metaFailed));
    log(tr("Downloading %1 files…").arg(jobs.size()));
    if (skippedMd5 > 0)
        log(tr("%1 already on disk (matched by content, skipped).").arg(skippedMd5));
    if (skippedDuplicate > 0)
        log(tr("%1 duplicate row(s) in this backup sharing a file (skipped).")
                .arg(skippedDuplicate));
    m_expectingDownloads = true;
    m_downloads->enqueue(jobs);
}

namespace {
QString humanSize(qint64 bytes) {
    if (bytes < 0)
        return QStringLiteral("?");
    static const char *units[] = {"B", "KB", "MB", "GB"};
    double v = static_cast<double>(bytes);
    int u = 0;
    while (v >= 1024.0 && u < 3) {
        v /= 1024.0;
        u++;
    }
    return QString::number(v, 'f', u == 0 ? 0 : 1) + u' ' +
           QString::fromLatin1(units[u]);
}
} // namespace

void LosnImportDialog::onFileProgress(const QString &filePath, qint64 received,
                                      qint64 total) {
    if (!m_expectingDownloads)
        return;
    const int pct = total > 0 ? static_cast<int>(received * 100 / total) : -1;
    auto it = m_fileRows.find(filePath);
    // One row per active file, refreshed only when the percent ticks:
    // QListWidget stays smooth even with hundreds of queued files.
    if (it != m_fileRows.end() && m_filePct.value(filePath, -2) == pct)
        return;
    const QString name = QFileInfo(filePath).fileName();
    QString text;
    if (pct >= 0)
        text = tr("%1 - %2% (%3 / %4)")
                   .arg(name)
                   .arg(pct)
                   .arg(humanSize(received), humanSize(total));
    else
        text = tr("%1 - %2").arg(name, humanSize(received));
    if (it == m_fileRows.end()) {
        auto *item = new QListWidgetItem(text, m_log);
        item->setData(Qt::UserRole, name);
        m_fileRows.insert(filePath, item);
    } else {
        (*it)->setText(text);
    }
    m_filePct.insert(filePath, pct);
    m_log->scrollToBottom();
}

void LosnImportDialog::onFileRenamed(const QString &oldName,
                                       const QString &newName) {
    // The manager hashed an md5-less file and renamed it: retitle the
    // live row so the log never shows stale _losn names.
    const QString oldPath = m_jobPaths.take(oldName);
    if (oldPath.isEmpty())
        return;
    const QString newPath =
        QFileInfo(oldPath).absolutePath() + u'/' + newName;
    m_jobPaths.insert(newName, newPath);
    auto it = m_fileRows.find(oldPath);
    if (it == m_fileRows.end())
        return;
    QListWidgetItem *item = it.value();
    m_fileRows.erase(it);
    m_fileRows.insert(newPath, item);
    m_filePct.remove(oldPath);
    item->setData(Qt::UserRole, newName);
    item->setText(tr("%1 - done").arg(newName));
}

void LosnImportDialog::onDownloadsDone(const DownloadSummary &summary) {
    // Fired for our batch (other batches are refused while we run).
    if (!m_expectingDownloads)
        return;
    m_expectingDownloads = false;
    setBusy(false);
    m_bar->setValue(m_bar->maximum());
    // Final on-disk paths (the manager renames md5-less files to
    // <Site>_<md5> after hashing - targetPaths() would miss those).
    QHash<QPair<QString, qint64>, QString> donePaths;
    QHash<QString, QString> byUrl;
    for (const auto &[url, path] : summary.finalPaths)
        byUrl.insert(url, path);
    for (const LosnRow &r : m_downloadRows) {
        if (r.fileUrl.isEmpty())
            continue;
        QString path = byUrl.value(r.fileUrl);
        if (path.isEmpty())
            path = m_diskPaths.value(r.fileUrl);
        if (path.isEmpty()) {
            // Skipped (already on disk) or predates finalPaths: fall back.
            path = LosnImport::targetPaths(r, m_downloadDir).filePath;
            if (!QFile::exists(path))
                continue;
        }
        donePaths.insert({r.siteLabel, r.postId}, path);
    }
    m_favDb->setLocalPaths(donePaths);
    for (QListWidgetItem *item : std::as_const(m_fileRows))
        item->setText(item->data(Qt::UserRole).toString() +
                      tr(" - done"));
    m_fileRows.clear();
    m_filePct.clear();
    log(tr("Downloads finished - rescan the download folder to browse them."));
    // Summary diff: which final files were genuinely new (md5 not on
    // disk before the run) vs already present. `done` counts downloads
    // in this batch; skipped/failed come from the manager's summary.
    const LosnImport::SummaryDiff diff =
        LosnImport::diffNewFiles(m_beforeMd5, summary.finalPaths);
    log(tr("Summary: %1 new file(s), %2 already present, %3 failed.")
            .arg(diff.newFiles.size())
            .arg(diff.alreadyPresent.size())
            .arg(summary.failed));
    if (!diff.newFiles.isEmpty()) {
        log(tr("New files:"));
        const int shown = qMin(diff.newFiles.size(), 50);
        for (int i = 0; i < shown; ++i)
            log(tr("  + %1").arg(diff.newFiles.at(i)));
        if (diff.newFiles.size() > shown)
            log(tr("  + %1 more…").arg(diff.newFiles.size() - shown));
    }
    if (m_settings.wallAutoPopup && !summary.wallTargets.isEmpty()) {
        const auto &wt = summary.wallTargets.first();
        emit wallSolveRequested(wt.first, QUrl(wt.second));
    }
}
