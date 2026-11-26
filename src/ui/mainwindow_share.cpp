#include "mainwindow.h"

#include "favdb.h"
#include "hydrusstore.h"
#include "imagecache.h"
#include "remotesource.h"
#include "scanner.h"
#include "sharecatalog.h"
#include "sharedialog.h"
#include "shareserver.h"
#include "uicons.h"

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QMessageBox>
#include <QNetworkRequest>
#include <QProgressDialog>
#include <QPushButton>
#include <QStatusBar>
#include <QTimer>
#include <QtConcurrent>

void MainWindow::openShareDialog() {
    if (m_shareProgress)
        return; // one build at a time

    bool hydrusAvailable = m_settings.hydrusEnabled;
    int hydrusCount = 0;
    if (hydrusAvailable) {
        hydrusAvailable = ensureHydrusDb();
        if (hydrusAvailable)
            hydrusCount = m_hydrusDb.count();
    }

    ShareOptions current;
    current.hydrus = m_settings.shareHydrus;
    current.downloads = m_settings.shareDownloads;
    current.favourites = m_settings.shareFavourites;
    current.localFolder = m_settings.shareLocalFolder;
    current.localFolderPath = m_settings.shareFolder;
    current.bindAddress = m_settings.shareBind;
    current.port = m_settings.sharePort;

    // First run with no remembered sources: default to what the user is
    // looking at, still showing the picker.
    if (!current.hydrus && !current.downloads && !current.favourites &&
        !current.localFolder) {
        if (BrowserTab *t = currentTab()) {
            if (t->isHydrus) {
                if (hydrusAvailable)
                    current.hydrus = true;
            } else if (t->isFavourites) {
                current.favourites = true;
            } else if (isDownloadsTab()) {
                current.downloads = true;
            } else if (!t->isRemote) {
                current.localFolder = true;
                if (current.localFolderPath.isEmpty())
                    current.localFolderPath = t->root;
            }
        }
    }

    const bool running = m_shareServer && m_shareServer->isRunning();
    const bool webUi = running && m_shareServer->hasWebUi();
    ShareDialog dlg(current, hydrusAvailable, hydrusCount, running, webUi,
                    m_shareServer ? m_shareServer->postCount() : 0,
                    m_shareServer ? m_shareServer->urls() : QStringList(),
                    this);
    if (dlg.exec() != QDialog::Accepted)
        return;

    const ShareOptions opts = dlg.options();
    const QString previousBind = m_settings.shareBind;
    m_settings.shareBind = opts.bindAddress;
    m_settings.sharePort = opts.port;
    m_settings.shareHydrus = opts.hydrus;
    m_settings.shareDownloads = opts.downloads;
    m_settings.shareFavourites = opts.favourites;
    m_settings.shareLocalFolder = opts.localFolder;
    m_settings.shareFolder = opts.localFolderPath;
    saveSettings(m_settings);

    switch (dlg.action()) {
    case ShareDialog::Action::Stop:
        stopShareServer();
        return;
    case ShareDialog::Action::Start:
        // Restart applies a changed bind address/port; a plain first start
        // has no listener yet.
        if (m_shareServer && m_shareServer->isRunning())
            stopShareServer();
        startShareBuild(opts);
        return;
    case ShareDialog::Action::Refresh:
        if (m_shareServer && m_shareServer->isRunning() &&
            (opts.bindAddress != previousBind ||
             opts.port != m_shareServer->port()))
            stopShareServer();
        startShareBuild(opts);
        return;
    case ShareDialog::Action::None:
        return;
    }
}

void MainWindow::startShareBuild(const ShareOptions &opts) {
    if (m_shareProgress)
        return;
    ++m_shareGeneration;
    const qint64 gen = m_shareGeneration;

    m_shareProgress = new QProgressDialog(
        tr("Building the share index…"), tr("Cancel"), 0, 0, this);
    m_shareProgress->setWindowTitle(tr("Share library on LAN"));
    m_shareProgress->setWindowModality(Qt::WindowModal);
    m_shareProgress->setCancelButtonText(tr("Cancel"));
    m_shareProgress->setMinimumDuration(0);

    connect(m_shareProgress, &QProgressDialog::canceled, this, [this] {
        ++m_shareGeneration;
        if (m_shareProgress) {
            m_shareProgress->deleteLater();
            m_shareProgress = nullptr;
        }
        if (m_shareScanner) {
            m_shareScanner->requestCancel();
            m_shareScanner.reset();
        }
        if (m_shareWatcher) {
            m_shareWatcher->deleteLater();
            m_shareWatcher = nullptr;
        }
        statusBar()->showMessage(tr("Share index build cancelled"), 4000);
    });
    m_shareProgress->show();
    QApplication::processEvents();

    auto catalog = std::make_shared<ShareCatalog>();

    if (opts.favourites && m_favDb.isOpen()) {
        const auto paths = m_favDb.localPaths();
        const QVector<Post> rows = m_favDb.list();
        int n = 0;
        for (const Post &row : rows) {
            QString local = row.absFile;
            if (local.isEmpty())
                local = paths.value(favIdentity(row));
            if (!local.isEmpty() && !QFileInfo::exists(local))
                local.clear();
            QString remote = row.fileUrl;
            if (remote.isEmpty())
                remote = row.sampleUrl;
            catalog->add(row, local, remote);
            if (++n % 100 == 0) {
                QApplication::processEvents();
                if (!m_shareProgress)
                    return;
            }
        }
    }
    if (opts.hydrus && ensureHydrusDb()) {
        const QString dir = effectiveHydrusDir(m_settings);
        const QVector<HydrusStore::Record> rows = m_hydrusDb.list();
        int n = 0;
        for (const HydrusStore::Record &row : rows) {
            Post p = row.toPost(dir);
            catalog->add(p, p.absFile);
            if (++n % 100 == 0) {
                QApplication::processEvents();
                if (!m_shareProgress)
                    return;
            }
        }
    }

    QStringList scanDirs;
    if (opts.downloads) {
        const QString dir = effectiveDownloadDir(m_settings);
        if (!dir.isEmpty())
            scanDirs << dir;
    }
    if (opts.localFolder && !opts.localFolderPath.trimmed().isEmpty())
        scanDirs << opts.localFolderPath.trimmed();
    scanDirs.removeDuplicates();

    if (scanDirs.isEmpty()) {
        if (m_shareProgress) {
            // Never close(): QProgressDialog emits canceled() from close(),
            // which would re-enter the cancel handler and null the pointer
            // mid-flight.
            m_shareProgress->hide();
            m_shareProgress->deleteLater();
            m_shareProgress = nullptr;
        }
        finishShareBuild(catalog, opts);
        return;
    }

    auto *watcher = new QFutureWatcher<QVector<Post>>(this);
    m_shareWatcher = watcher;
    const auto scanner = QSharedPointer<Scanner>::create();
    m_shareScanner = scanner;
    connect(watcher, &QFutureWatcher<QVector<Post>>::finished, this,
            [this, watcher, catalog, opts, gen] {
                if (watcher != m_shareWatcher)
                    return; // cancelled/replaced; the watcher is gone
                if (gen != m_shareGeneration) {
                    m_shareWatcher = nullptr;
                    watcher->deleteLater();
                    return;
                }
                m_shareWatcher = nullptr;
                m_shareScanner.reset();
                const QVector<Post> posts = watcher->result();
                watcher->deleteLater();
                for (const Post &p : posts) {
                    if (!p.absFile.isEmpty())
                        catalog->add(p, p.absFile);
                }
                if (m_shareProgress) {
                    m_shareProgress->hide();
                    m_shareProgress->deleteLater();
                    m_shareProgress = nullptr;
                }
                finishShareBuild(catalog, opts);
            });
    watcher->setFuture(QtConcurrent::run([scanDirs, scanner] {
        QVector<Post> all;
        for (const QString &dir : scanDirs) {
            if (scanner->cancelRequested.load() || !QDir(dir).exists())
                continue;
            const QVector<Post> posts =
                Scanner::scan(dir, scanner.data(), false, nullptr);
            for (const Post &p : posts) {
                if (!p.absFile.isEmpty())
                    all << p;
            }
        }
        return all;
    }));
}

void MainWindow::finishShareBuild(
    const std::shared_ptr<ShareCatalog> &catalog, const ShareOptions &opts) {
    if (catalog->size() == 0) {
        QMessageBox::information(
            this, tr("Share library"),
            tr("No shareable posts were found in the selected sources."));
        return;
    }
    if (m_shareServer && m_shareServer->isRunning()) {
        m_shareServer->setCatalog(catalog);
        m_shareCatalog = catalog;
        updateShareStatus();
        statusBar()->showMessage(
            tr("Share snapshot updated: %1 post(s)").arg(catalog->size()),
            6000);
        return;
    }

    auto *server = new ShareServer(this);
    server->setCatalog(catalog);
    server->setMediaResolver(
        [this](const ShareItem &item, ShareServer::MediaReady done) {
            resolveShareMedia(item, std::move(done));
        });
    server->setRequestDecorator([](QNetworkRequest &req) {
        RemoteSource::applyCookies(req);
        req.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("box/0.14 (share)"));
    });
    server->setNetworkProxy(RemotePosts::proxyFromSpec(QString()));
    QString error;
    if (!server->start(opts.bindAddress, opts.port,
                       appCacheDir() + QStringLiteral("/share-thumbs"),
                       &error)) {
        delete server;
        QMessageBox::warning(
            this, tr("Share library"),
            error.isEmpty() ? tr("Could not start the API server.")
                            : tr("Could not start the API server:\n%1")
                                  .arg(error));
        return;
    }
    m_shareServer = server;
    m_shareCatalog = catalog;
    updateShareStatus();
    const QString first = server->urls().value(0);
    statusBar()->showMessage(
        tr("Share is live: %1 post(s) at %2")
            .arg(catalog->size())
            .arg(first),
        12000);
}

void MainWindow::stopShareServer() {
    ++m_shareGeneration;
    if (m_shareProgress) {
        m_shareProgress->hide();
        m_shareProgress->deleteLater();
        m_shareProgress = nullptr;
    }
    if (m_shareScanner) {
        m_shareScanner->requestCancel();
        m_shareScanner.reset();
    }
    if (m_shareWatcher) {
        m_shareWatcher->deleteLater();
        m_shareWatcher = nullptr;
    }
    if (m_shareServer) {
        m_shareServer->stop();
        m_shareServer->deleteLater();
        m_shareServer = nullptr;
    }
    m_shareCatalog.reset();
    updateShareStatus();
    statusBar()->showMessage(tr("LAN sharing stopped"), 4000);
}

void MainWindow::updateShareStatus() {
    if (!m_shareStatusBtn)
        return;
    const bool running = m_shareServer && m_shareServer->isRunning();
    m_shareStatusBtn->setVisible(running);
    if (!running)
        return;
    const QStringList urls = m_shareServer->urls();
    const bool web = m_shareServer->hasWebUi();
    m_shareStatusBtn->setText(tr("LIVE"));
    const QString url = urls.value(0);
    QString tip = tr("%1 post(s) shared read-only.\n\n")
                      .arg(m_shareServer->postCount());
    if (web) {
        tip += tr("Web UI: %1\n").arg(url);
        tip += tr("Danbooru API: %1/posts.json\n").arg(url);
    } else {
        tip += tr("API: %1/posts.json\n").arg(url);
        tip += tr("The Box Share web UI is not embedded in this build; "
                  "the server root shows a plain page.\n");
    }
    tip += tr("\nClick to manage");
    if (urls.size() > 1)
        tip += tr("\n\nAll addresses:\n%1").arg(urls.join(u'\n'));
    m_shareStatusBtn->setToolTip(tip);
    m_shareStatusBtn->setAccessibleName(
        web ? tr("Box Share is live (web UI and API)")
            : tr("Box Share API is live"));
}

void MainWindow::resolveShareMedia(
    const ShareItem &item,
    std::function<void(const QString &localPath, const QUrl &remoteUrl,
                       const QString &error)> done) {
    if (!item.localPath.isEmpty() &&
        QFileInfo(item.localPath).isFile()) {
        done(item.localPath, QUrl(), QString());
        return;
    }
    QString url = item.remoteMediaUrl;
    if (url.isEmpty())
        url = item.post.fileUrl;
    if (url.isEmpty())
        url = item.post.sampleUrl;
    if (url.isEmpty()) {
        done(QString(), QUrl(), tr("No media URL for this post"));
        return;
    }
    const Post post = item.post;
    if (isVideoExt(post.ext) || !RemotePosts::isImageUrl(url)) {
        done(QString(), QUrl(url), QString());
        return;
    }
    if (!m_imgCache) {
        done(QString(), QUrl(), tr("Media cache unavailable"));
        return;
    }
    const QString cached = m_imgCache->localPath(url, &post);
    if (!cached.isEmpty()) {
        done(cached, QUrl(), QString());
        return;
    }
    auto readyConn = std::make_shared<QMetaObject::Connection>();
    auto failConn = std::make_shared<QMetaObject::Connection>();
    *readyConn = connect(
        m_imgCache, &ImageCache::ready, this,
        [readyConn, failConn, url, done](const QString &readyUrl,
                                         const QString &path) {
            if (readyUrl != url)
                return;
            QObject::disconnect(*readyConn);
            QObject::disconnect(*failConn);
            done(path, QUrl(), QString());
        });
    *failConn = connect(
        m_imgCache, &ImageCache::failed, this,
        [readyConn, failConn, url, done](const QString &failedUrl,
                                         const QString &error) {
            if (failedUrl != url)
                return;
            QObject::disconnect(*readyConn);
            QObject::disconnect(*failConn);
            done(QString(), QUrl(), error);
        });
    m_imgCache->request(url, RemotePosts::mediaRefererFor(post), &post);
}
