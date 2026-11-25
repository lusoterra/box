#pragma once

// Read-only LAN HTTP server for a ShareCatalog. It speaks the Danbooru
// JSON shape (/posts.json, /posts/<id>.json, /tags.json and
// /counts/posts.json) so LoliSnatcher or another box instance can add it
// as a normal Danbooru entry. Media is served at /share/file/<id>,
// thumbnails at /share/thumb/<id>. Nothing is writable.
#include "sharecatalog.h"

#include <QByteArray>
#include <QNetworkProxy>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <functional>
#include <memory>

class QNetworkAccessManager;
class QNetworkRequest;
class QTcpServer;
class QNetworkReply;
class ShareConnection;

class ShareServer : public QObject {
    Q_OBJECT
public:
    // A remote-only item is resolved to either a local file (cache/temp)
    // or a remote URL that the server streams through. Exactly one should
    // be set; `error` stops the request.
    using MediaReady = std::function<void(const QString &localPath,
                                          const QUrl &remoteUrl,
                                          const QString &error)>;
    using ResolveMedia = std::function<void(const ShareItem &, MediaReady)>;
    // Adds cookies / a user agent to outbound proxy requests.
    using RequestDecorator = std::function<void(QNetworkRequest &)>;

    explicit ShareServer(QObject *parent = nullptr);
    ~ShareServer() override;

    bool start(const QString &bindAddress, int port,
               const QString &thumbCacheDir, QString *error = nullptr);
    void stop();
    bool isRunning() const;
    // True when the single-file Box Share UI is embedded. The same root
    // URL serves it; without it the root renders the plain API fallback.
    bool hasWebUi() const;
    int port() const { return m_port; }
    int postCount() const;
    // http://<lan-ip>:<port> entries, loopback included last.
    QStringList urls() const;

    // Replacing the catalog is safe while connections are active: each
    // request holds the snapshot it started with.
    void setCatalog(std::shared_ptr<const ShareCatalog> catalog);
    void setMediaResolver(ResolveMedia resolver);
    void setRequestDecorator(RequestDecorator decorator);
    void setNetworkProxy(const QNetworkProxy &proxy);

private:
    friend class ShareConnection;
    void onNewConnection();

    QTcpServer *m_tcp = nullptr;
    QNetworkAccessManager *m_nam = nullptr;
    std::shared_ptr<const ShareCatalog> m_catalog;
    ResolveMedia m_resolve;
    RequestDecorator m_decorate;
    QString m_thumbDir;
    QString m_bind;
    int m_port = 0;
};
