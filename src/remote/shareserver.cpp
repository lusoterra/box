#include "shareserver.h"

#include "post.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QHostAddress>
#include <QImage>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeDatabase>
#include <QMutex>
#include <QNetworkAccessManager>
#include <QNetworkInterface>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QPointer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrlQuery>
#include <QUuid>
#include <QtConcurrent>

#include <algorithm>

namespace {

QString mimeForFile(const QFileInfo &fi) {
    QMimeDatabase db;
    QString type = db.mimeTypeForFile(fi).name();
    if (!type.isEmpty())
        return type;
    const QString ext = fi.suffix().toLower();
    if (ext == QStringLiteral("jxl"))
        return QStringLiteral("image/jxl");
    if (ext == QStringLiteral("webp"))
        return QStringLiteral("image/webp");
    if (ext == QStringLiteral("avif"))
        return QStringLiteral("image/avif");
    return QStringLiteral("application/octet-stream");
}

// One cached local thumbnail. Video files get a placeholder; decoding a
// frame here would drag a media player into the server. The booru preview
// URL is used in the JSON for downloaded videos that have one.
QString makeThumbPlaceholder(const QString &cacheDir, bool video) {
    if (cacheDir.isEmpty())
        return {};
    const QString path = cacheDir + QStringLiteral("/placeholder-") +
                         (video ? QStringLiteral("video") : QStringLiteral("file")) +
                         QStringLiteral(".jpg");
    if (QFileInfo(path).size() > 0)
        return path;
    QImage img(360, 360, QImage::Format_ARGB32);
    img.fill(QColor(0x20, 0x22, 0x2a));
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(QColor(0x8a, 0x90, 0xa8), 4));
    p.drawRoundedRect(QRectF(18, 18, 324, 324), 16, 16);
    if (video) {
        QPolygonF tri;
        tri << QPointF(145, 110) << QPointF(245, 180) << QPointF(145, 250);
        p.setBrush(QColor(0x8a, 0x90, 0xa8));
        p.setPen(Qt::NoPen);
        p.drawPolygon(tri);
    } else {
        p.drawRect(QRectF(118, 118, 124, 124));
        p.drawLine(QPointF(118, 168), QPointF(242, 168));
    }
    p.end();
    const QString tmp = path + QStringLiteral(".part-") +
                        QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (!img.save(tmp, "JPEG", 85)) {
        QFile::remove(tmp);
        return {};
    }
    QFile::remove(path);
    if (!QFile::rename(tmp, path)) {
        QFile::remove(tmp);
        return {};
    }
    return path;
}

QString makeShareThumb(const QString &path, const QString &cacheDir,
                       bool video) {
    if (cacheDir.isEmpty())
        return {};
    if (video)
        return makeThumbPlaceholder(cacheDir, true);
    const QFileInfo fi(path);
    if (!fi.exists() || !fi.isFile())
        return makeThumbPlaceholder(cacheDir, false);
    const QString key =
        QCryptographicHash::hash(
            (QFileInfo(path).canonicalFilePath() + u'|' +
             QString::number(fi.lastModified().toMSecsSinceEpoch()) + u'|' +
             QString::number(fi.size()) + QStringLiteral("|360"))
                .toUtf8(),
            QCryptographicHash::Sha1)
            .toHex();
    const QString out = cacheDir + u'/' + key + QStringLiteral(".jpg");
    if (QFileInfo(out).size() > 0)
        return out;

    QImageReader reader(path);
    reader.setAutoTransform(true);
    if (!reader.canRead())
        return makeThumbPlaceholder(cacheDir, false);
    const QSize src = reader.size();
    if (src.isValid() && src.width() > 0 && src.height() > 0)
        reader.setScaledSize(src.scaled(360, 360, Qt::KeepAspectRatio));
    QImage img = reader.read();
    if (img.isNull())
        return makeThumbPlaceholder(cacheDir, false);
    if (img.width() > 360 || img.height() > 360)
        img = img.scaled(360, 360, Qt::KeepAspectRatio,
                         Qt::SmoothTransformation);
    QImage opaque(img.size(), QImage::Format_RGB32);
    opaque.fill(QColor(0x20, 0x22, 0x2a));
    {
        QPainter p(&opaque);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        if (img.hasAlphaChannel()) {
            p.drawImage(0, 0, img);
        } else {
            p.drawImage(0, 0, img);
        }
    }
    const QString tmp = out + QStringLiteral(".part-") +
                        QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (!opaque.save(tmp, "JPEG", 85)) {
        QFile::remove(tmp);
        return makeThumbPlaceholder(cacheDir, false);
    }
    QFile::remove(out);
    if (!QFile::rename(tmp, out)) {
        QFile::remove(tmp);
        return makeThumbPlaceholder(cacheDir, false);
    }
    return out;
}

QByteArray statusText(int status) {
    switch (status) {
    case 200: return "OK";
    case 206: return "Partial Content";
    case 302: return "Found";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 416: return "Range Not Satisfiable";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 502: return "Bad Gateway";
    default: return "Error";
    }
}

QByteArray jsonError(const QString &message) {
    QJsonObject o;
    o[QStringLiteral("success")] = false;
    o[QStringLiteral("message")] = message;
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

} // namespace

class ShareConnection : public QObject {
public:
    ShareConnection(QTcpSocket *socket, ShareServer *server)
        : QObject(server), m_socket(socket), m_server(server) {
        m_socket->setParent(this);
        connect(m_socket, &QTcpSocket::readyRead, this,
                [this] { onReadyRead(); });
        connect(m_socket, &QTcpSocket::disconnected, this,
                [this] { deleteLater(); });
        connect(m_socket, &QTcpSocket::bytesWritten, this,
                [this] { pumpFile(); });
    }
    ~ShareConnection() override {
        if (m_reply) {
            m_reply->abort();
            m_reply = nullptr;
        }
    }

private:
    void onReadyRead();
    void handle();
    void handlePosts(const QUrlQuery &query);
    void handlePost(qint64 id);
    void handleTags(const QUrlQuery &query);
    void handleCounts(const QUrlQuery &query);
    void handleMedia(qint64 id);
    void handleThumb(qint64 id);
    void finishMedia(const ShareItem &item, const QString &localPath,
                     const QUrl &remoteUrl, const QString &error);
    void sendLocalFile(const QString &path);
    void pumpFile();
    void proxyRemote(const ShareItem &item, const QUrl &url);
    void sendProxyHeaders();
    void respond(int status, const QByteArray &contentType,
                 const QByteArray &body, const QByteArray &extra = {},
                 bool cache = false);
    void respondJson(int status, const QJsonDocument &doc);
    void respondJson(int status, const QJsonArray &arr);
    void respondJson(int status, const QJsonObject &obj);
    void respondError(int status, const QString &message);
    void redirect(const QUrl &url);
    qint64 parseId(const QString &path, const QString &prefix) const;
    QString baseUrl() const;

    QTcpSocket *m_socket = nullptr;
    ShareServer *m_server = nullptr;
    QByteArray m_buffer;
    QHash<QByteArray, QByteArray> m_headers;
    bool m_handled = false;
    bool m_head = false;
    QString m_baseUrl;
    QFile *m_file = nullptr;
    qint64 m_fileRemaining = 0;
    QNetworkReply *m_reply = nullptr;
    bool m_proxyHeaders = false;
    bool m_done = false;
};

void ShareConnection::onReadyRead() {
    if (m_handled)
        return;
    m_buffer += m_socket->readAll();
    const int end = m_buffer.indexOf("\r\n\r\n");
    if (end < 0) {
        if (m_buffer.size() > 64 * 1024) {
            m_handled = true;
            respondError(431, tr("Request headers too large"));
        }
        return;
    }
    m_buffer.truncate(end);
    m_handled = true;
    handle();
}

void ShareConnection::handle() {
    const QList<QByteArray> lines = m_buffer.split('\n');
    if (lines.isEmpty()) {
        respondError(400, tr("Empty request"));
        return;
    }
    const QList<QByteArray> request = lines.first().trimmed().split(' ');
    if (request.size() < 2) {
        respondError(400, tr("Malformed request"));
        return;
    }
    const QByteArray method = request.at(0).trimmed().toUpper();
    const QByteArray target = request.at(1).trimmed();
    if (method != "GET" && method != "HEAD") {
        m_head = method == "HEAD";
        respond(405, "application/json",
                jsonError(tr("Only GET and HEAD are supported")),
                "Allow: GET, HEAD\r\n");
        return;
    }
    m_head = method == "HEAD";
    for (int i = 1; i < lines.size(); ++i) {
        const QByteArray line = lines.at(i).trimmed();
        if (line.isEmpty())
            continue;
        const int colon = line.indexOf(':');
        if (colon <= 0)
            continue;
        m_headers.insert(line.left(colon).trimmed().toLower(),
                         line.mid(colon + 1).trimmed());
    }
    m_baseUrl = baseUrl();

    QUrl url = QUrl::fromEncoded(target);
    if (url.path().isEmpty())
        url.setPath(QStringLiteral("/"));
    const QString path = url.path();
    const QUrlQuery query(url);

    if (path == QStringLiteral("/")) {
        const QString html =
            QStringLiteral(
                "<!doctype html><meta charset=\"utf-8\"><title>box share</title>"
                "<h2>box - read-only library</h2><p>%1 posts.</p>"
                "<ul><li><a href=\"/posts.json\">/posts.json</a></li>"
                "<li><a href=\"/tags.json\">/tags.json</a></li>"
                "<li><a href=\"/counts/posts.json\">/counts/posts.json</a></li>"
                "</ul><p>Add this URL as a Danbooru instance.</p>")
                .arg(m_server->postCount());
        respond(200, "text/html; charset=utf-8", html.toUtf8());
        return;
    }
    if (path == QStringLiteral("/posts.json")) {
        handlePosts(query);
        return;
    }
    if (path == QStringLiteral("/tags.json") ||
        path == QStringLiteral("/tags/autocomplete.json") ||
        path == QStringLiteral("/autocomplete.json")) {
        handleTags(query);
        return;
    }
    if (path == QStringLiteral("/counts/posts.json")) {
        handleCounts(query);
        return;
    }
    if (path.startsWith(QStringLiteral("/posts/")) &&
        path.endsWith(QStringLiteral(".json"))) {
        const QString idPart =
            path.mid(7, path.size() - 7 - 5);
        bool ok = false;
        const qint64 id = idPart.toLongLong(&ok);
        if (!ok) {
            respondError(400, tr("Bad post id"));
            return;
        }
        handlePost(id);
        return;
    }
    if (path.startsWith(QStringLiteral("/posts/"))) {
        const qint64 id = parseId(path, QStringLiteral("/posts/"));
        const auto *catalog = m_server->m_catalog.get();
        const ShareItem *item = catalog ? catalog->byId(id) : nullptr;
        if (!item) {
            respondError(404, tr("No such post"));
            return;
        }
        redirect(QUrl(m_baseUrl + QStringLiteral("/share/file/") +
                      QString::number(id)));
        return;
    }
    if (path.startsWith(QStringLiteral("/share/file/"))) {
        const qint64 id = parseId(path, QStringLiteral("/share/file/"));
        if (id <= 0) {
            respondError(400, tr("Bad post id"));
            return;
        }
        handleMedia(id);
        return;
    }
    if (path.startsWith(QStringLiteral("/share/thumb/"))) {
        const qint64 id = parseId(path, QStringLiteral("/share/thumb/"));
        if (id <= 0) {
            respondError(400, tr("Bad post id"));
            return;
        }
        handleThumb(id);
        return;
    }
    respondError(404, tr("Unknown endpoint"));
}

qint64 ShareConnection::parseId(const QString &path,
                                const QString &prefix) const {
    QString rest = path.mid(prefix.size());
    const qsizetype dot = rest.indexOf(u'.');
    if (dot >= 0)
        rest = rest.left(dot);
    bool ok = false;
    const qint64 id = rest.toLongLong(&ok);
    return ok ? id : 0;
}

QString ShareConnection::baseUrl() const {
    QString host = QString::fromLatin1(m_headers.value("host")).trimmed();
    QString proto =
        QString::fromLatin1(m_headers.value("x-forwarded-proto")).trimmed();
    if (proto.compare(QStringLiteral("https"), Qt::CaseInsensitive) == 0)
        proto = QStringLiteral("https");
    else
        proto = QStringLiteral("http");
    if (host.isEmpty()) {
        const QStringList urls = m_server->urls();
        if (!urls.isEmpty())
            return urls.first();
        return QStringLiteral("http://127.0.0.1:%1")
            .arg(m_server->port());
    }
    return proto + QStringLiteral("://") + host;
}

void ShareConnection::handlePosts(const QUrlQuery &query) {
    const auto *catalog = m_server->m_catalog.get();
    if (!catalog) {
        respondError(500, tr("No catalog"));
        return;
    }
    const QString tags = query.queryItemValue(QStringLiteral("tags"),
                                              QUrl::FullyDecoded)
                             .replace(u'+', u' ');
    int page = query.queryItemValue(QStringLiteral("page")).toInt();
    int limit = query.queryItemValue(QStringLiteral("limit")).toInt();
    if (page <= 0)
        page = 1;
    if (limit <= 0)
        limit = 40;
    const QVector<const ShareItem *> items =
        catalog->search(tags, page, limit);
    respondJson(200, catalog->postsJson(items, m_baseUrl));
}

void ShareConnection::handlePost(qint64 id) {
    const auto *catalog = m_server->m_catalog.get();
    const ShareItem *item = catalog ? catalog->byId(id) : nullptr;
    if (!item) {
        respondError(404, tr("No such post"));
        return;
    }
    respondJson(200, catalog->postJson(*item, m_baseUrl));
}

void ShareConnection::handleTags(const QUrlQuery &query) {
    const auto *catalog = m_server->m_catalog.get();
    if (!catalog) {
        respondError(500, tr("No catalog"));
        return;
    }
    QString prefix =
        query.queryItemValue(QStringLiteral("search[name_matches]"),
                             QUrl::FullyDecoded);
    if (prefix.isEmpty())
        prefix = query.queryItemValue(QStringLiteral("name_matches"),
                                      QUrl::FullyDecoded);
    if (prefix.isEmpty())
        prefix =
            query.queryItemValue(QStringLiteral("query"), QUrl::FullyDecoded);
    if (prefix.isEmpty())
        prefix =
            query.queryItemValue(QStringLiteral("name"), QUrl::FullyDecoded);
    int limit = query.queryItemValue(QStringLiteral("limit")).toInt();
    if (limit <= 0)
        limit = 64;
    respondJson(200, catalog->tagsJson(prefix, limit));
}

void ShareConnection::handleCounts(const QUrlQuery &query) {
    const auto *catalog = m_server->m_catalog.get();
    if (!catalog) {
        respondError(500, tr("No catalog"));
        return;
    }
    const QString tags = query.queryItemValue(QStringLiteral("tags"),
                                              QUrl::FullyDecoded)
                             .replace(u'+', u' ');
    QJsonObject counts;
    counts[QStringLiteral("posts")] = catalog->count(tags);
    QJsonObject o;
    o[QStringLiteral("counts")] = counts;
    respondJson(200, o);
}

void ShareConnection::handleMedia(qint64 id) {
    const auto *catalog = m_server->m_catalog.get();
    const ShareItem *item = catalog ? catalog->byId(id) : nullptr;
    if (!item) {
        respondError(404, tr("No such post"));
        return;
    }
    if (!item->localPath.isEmpty() &&
        QFileInfo(item->localPath).isFile()) {
        sendLocalFile(item->localPath);
        return;
    }
    if (!m_server->m_resolve) {
        respondError(404, tr("No media for this post"));
        return;
    }
    const ShareItem itemCopy = *item;
    QPointer<ShareConnection> self(this);
    m_server->m_resolve(
        itemCopy,
        [self, itemCopy](const QString &localPath, const QUrl &remoteUrl,
                         const QString &error) {
            if (self)
                self->finishMedia(itemCopy, localPath, remoteUrl, error);
        });
}

void ShareConnection::finishMedia(const ShareItem &item,
                                  const QString &localPath,
                                  const QUrl &remoteUrl,
                                  const QString &error) {
    if (m_done)
        return;
    if (!error.isEmpty()) {
        respondError(502, error);
        return;
    }
    if (!localPath.isEmpty() && QFileInfo(localPath).isFile()) {
        sendLocalFile(localPath);
        return;
    }
    if (remoteUrl.isValid() && !remoteUrl.isEmpty()) {
        proxyRemote(item, remoteUrl);
        return;
    }
    respondError(404, tr("No media for this post"));
}

void ShareConnection::handleThumb(qint64 id) {
    const auto *catalog = m_server->m_catalog.get();
    const ShareItem *item = catalog ? catalog->byId(id) : nullptr;
    if (!item) {
        respondError(404, tr("No such post"));
        return;
    }
    const bool video = isVideoExt(item->post.ext);
    if (item->localPath.isEmpty()) {
        if (!item->post.previewUrl.isEmpty()) {
            redirect(QUrl(item->post.previewUrl));
            return;
        }
        if (!item->remotePreviewUrl.isEmpty()) {
            redirect(QUrl(item->remotePreviewUrl));
            return;
        }
        const QString ph = makeThumbPlaceholder(m_server->m_thumbDir, video);
        if (ph.isEmpty()) {
            respondError(500, tr("Cannot make a placeholder"));
            return;
        }
        sendLocalFile(ph);
        return;
    }
    if (video) {
        if (!item->post.previewUrl.isEmpty()) {
            redirect(QUrl(item->post.previewUrl));
            return;
        }
        const QString ph = makeThumbPlaceholder(m_server->m_thumbDir, true);
        if (ph.isEmpty()) {
            respondError(500, tr("Cannot make a placeholder"));
            return;
        }
        sendLocalFile(ph);
        return;
    }
    auto *watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcher<QString>::finished, this,
            [this, watcher] {
                const QString path = watcher->result();
                watcher->deleteLater();
                if (path.isEmpty()) {
                    respondError(500, tr("Cannot make a thumbnail"));
                    return;
                }
                sendLocalFile(path);
            });
    watcher->setFuture(QtConcurrent::run(
        &makeShareThumb, item->localPath, m_server->m_thumbDir, false));
}

void ShareConnection::respond(int status, const QByteArray &contentType,
                              const QByteArray &body,
                              const QByteArray &extra, bool cache) {
    if (m_done || !m_socket)
        return;
    m_done = true;
    QByteArray head;
    head += "HTTP/1.1 " + QByteArray::number(status) + ' ' +
            statusText(status) + "\r\n";
    head += "Content-Type: " + contentType + "\r\n";
    head += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
    head += "Connection: close\r\n";
    head += "Access-Control-Allow-Origin: *\r\n";
    head += cache ? "Cache-Control: public, max-age=3600\r\n"
                  : "Cache-Control: no-store\r\n";
    head += extra;
    head += "\r\n";
    m_socket->write(head);
    if (!m_head)
        m_socket->write(body);
    m_socket->disconnectFromHost();
}

void ShareConnection::respondJson(int status, const QJsonArray &arr) {
    respond(status, "application/json; charset=utf-8",
            QJsonDocument(arr).toJson(QJsonDocument::Compact));
}

void ShareConnection::respondJson(int status, const QJsonObject &obj) {
    respond(status, "application/json; charset=utf-8",
            QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

void ShareConnection::respondJson(int status, const QJsonDocument &doc) {
    respond(status, "application/json; charset=utf-8",
            doc.toJson(QJsonDocument::Compact));
}

void ShareConnection::respondError(int status, const QString &message) {
    respond(status, "application/json; charset=utf-8",
            jsonError(message));
}

void ShareConnection::redirect(const QUrl &url) {
    if (m_done || !m_socket)
        return;
    m_done = true;
    QByteArray head =
        "HTTP/1.1 302 Found\r\n"
        "Location: " +
        url.toEncoded() +
        "\r\nContent-Length: 0\r\nConnection: close\r\n"
        "Access-Control-Allow-Origin: *\r\nCache-Control: no-store\r\n\r\n";
    m_socket->write(head);
    m_socket->disconnectFromHost();
}

void ShareConnection::sendLocalFile(const QString &path) {
    if (m_done || !m_socket)
        return;
    const QFileInfo fi(path);
    if (!fi.exists() || !fi.isFile()) {
        respondError(404, tr("File is gone"));
        return;
    }
    const qint64 size = fi.size();
    qint64 start = 0;
    qint64 end = size - 1;
    bool partial = false;
    const QByteArray range = m_headers.value("range").trimmed();
    if (range.toLower().startsWith("bytes=")) {
        QByteArray spec = range.mid(6);
        const int comma = spec.indexOf(',');
        if (comma >= 0)
            spec = spec.left(comma);
        const int dash = spec.indexOf('-');
        if (dash >= 0) {
            const QByteArray a = spec.left(dash).trimmed();
            const QByteArray b = spec.mid(dash + 1).trimmed();
            bool okA = true;
            bool okB = true;
            if (!a.isEmpty())
                start = a.toLongLong(&okA);
            if (!b.isEmpty())
                end = b.toLongLong(&okB);
            else
                end = size - 1;
            if (!a.isEmpty() && b.isEmpty() && okA) {
                // bytes=N- : open-ended
            } else if (a.isEmpty()) {
                // bytes=-N : last N bytes
                const qint64 n = b.toLongLong(&okB);
                if (okB && n > 0) {
                    start = qMax<qint64>(0, size - n);
                    end = size - 1;
                } else {
                    okA = false;
                }
            }
            if (!okA || !okB || start < 0 || end < start || start >= size) {
                respond(416, "text/plain", "range not satisfiable",
                        "Content-Range: bytes */" +
                            QByteArray::number(size) + "\r\n");
                return;
            }
            if (end >= size)
                end = size - 1;
            partial = true;
        }
    }
    const qint64 length = end - start + 1;
    const QByteArray type = mimeForFile(fi).toUtf8();
    QByteArray extra;
    extra += "Accept-Ranges: bytes\r\n";
    if (partial)
        extra += "Content-Range: bytes " + QByteArray::number(start) + '-' +
                 QByteArray::number(end) + '/' + QByteArray::number(size) +
                 "\r\n";
    if (m_done)
        return;
    if (m_head) {
        m_done = true;
        QByteArray head;
        head += "HTTP/1.1 " +
                QByteArray::number(partial ? 206 : 200) + ' ' +
                statusText(partial ? 206 : 200) + "\r\n";
        head += "Content-Type: " + type + "\r\n";
        head += "Content-Length: " + QByteArray::number(length) + "\r\n";
        head += "Connection: close\r\n";
        head += "Access-Control-Allow-Origin: *\r\n";
        head += "Cache-Control: public, max-age=3600\r\n";
        head += extra;
        head += "\r\n";
        m_socket->write(head);
        m_socket->disconnectFromHost();
        return;
    }
    m_file = new QFile(path, this);
    if (!m_file->open(QIODevice::ReadOnly) || !m_file->seek(start)) {
        m_file->close();
        delete m_file;
        m_file = nullptr;
        respondError(500, tr("Cannot read file"));
        return;
    }
    m_done = true;
    m_fileRemaining = length;
    QByteArray head;
    head += "HTTP/1.1 " +
            QByteArray::number(partial ? 206 : 200) + ' ' +
            statusText(partial ? 206 : 200) + "\r\n";
    head += "Content-Type: " + type + "\r\n";
    head += "Content-Length: " + QByteArray::number(length) + "\r\n";
    head += "Connection: close\r\n";
    head += "Access-Control-Allow-Origin: *\r\n";
    head += "Cache-Control: public, max-age=3600\r\n";
    head += extra;
    head += "\r\n";
    m_socket->write(head);
    pumpFile();
}

void ShareConnection::pumpFile() {
    if (!m_file || !m_socket) {
        if (m_fileRemaining == 0 && m_socket) {
            if (m_file)
                m_file->close();
            m_socket->disconnectFromHost();
        }
        return;
    }
    while (m_fileRemaining > 0 && m_socket->bytesToWrite() < (1 << 20)) {
        const QByteArray chunk =
            m_file->read(qMin<qint64>(m_fileRemaining, 256 * 1024));
        if (chunk.isEmpty()) {
            m_file->close();
            m_socket->abort();
            return;
        }
        m_fileRemaining -= chunk.size();
        m_socket->write(chunk);
    }
    if (m_fileRemaining <= 0) {
        m_file->close();
        m_socket->disconnectFromHost();
    }
}

void ShareConnection::proxyRemote(const ShareItem &item, const QUrl &url) {
    if (m_done || !m_socket)
        return;
    QNetworkRequest req(url);
    req.setTransferTimeout(30000);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setHeader(QNetworkRequest::UserAgentHeader,
                  QStringLiteral("box/0.11 (share)"));
    req.setRawHeader("Accept", "*/*");
    if (!m_headers.value("range").isEmpty())
        req.setRawHeader("Range", m_headers.value("range"));
    QString referer = item.post.postUrl;
    if (!referer.startsWith(QStringLiteral("http"), Qt::CaseInsensitive))
        referer = item.post.source;
    if (referer.startsWith(QStringLiteral("http"), Qt::CaseInsensitive))
        req.setRawHeader("Referer", referer.toUtf8());
    if (m_server->m_decorate)
        m_server->m_decorate(req);
    m_reply = m_server->m_nam->get(req);
    connect(m_reply, &QNetworkReply::metaDataChanged, this,
            [this] { sendProxyHeaders(); });
    connect(m_reply, &QNetworkReply::readyRead, this, [this] {
        if (!m_proxyHeaders)
            sendProxyHeaders();
        if (!m_reply)
            return;
        const QByteArray chunk = m_reply->readAll();
        if (m_proxyHeaders && !m_done && !m_head && !chunk.isEmpty())
            m_socket->write(chunk);
    });
    connect(m_reply, &QNetworkReply::finished, this, [this] {
        if (!m_reply)
            return;
        QNetworkReply *reply = m_reply;
        m_reply = nullptr;
        if (!m_proxyHeaders)
            sendProxyHeaders();
        if (m_head && reply)
            reply->abort();
        reply->deleteLater();
        if (m_socket)
            m_socket->disconnectFromHost();
    });
}

void ShareConnection::sendProxyHeaders() {
    if (m_proxyHeaders || m_done || !m_reply || !m_socket)
        return;
    const int status =
        m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status >= 300 && status < 400)
        return; // QNAM is following the redirect; wait for the final reply
    if (status >= 400 || m_reply->error() != QNetworkReply::NoError) {
        respondError(502, tr("Remote media failed"));
        m_reply->abort();
        return;
    }
    m_proxyHeaders = true;
    const QByteArray type =
        m_reply->header(QNetworkRequest::ContentTypeHeader).toByteArray();
    const qint64 length =
        m_reply->header(QNetworkRequest::ContentLengthHeader).toLongLong();
    QByteArray head;
    head += "HTTP/1.1 " + QByteArray::number(status == 206 ? 206 : 200) +
            ' ' + statusText(status == 206 ? 206 : 200) + "\r\n";
    head += "Content-Type: " +
            (type.isEmpty() ? QByteArray("application/octet-stream") : type) +
            "\r\n";
    if (length > 0)
        head += "Content-Length: " + QByteArray::number(length) + "\r\n";
    const QByteArray range =
        m_reply->rawHeader("Content-Range");
    if (!range.isEmpty())
        head += "Content-Range: " + range + "\r\n";
    if (!m_reply->rawHeader("Accept-Ranges").isEmpty())
        head += "Accept-Ranges: " + m_reply->rawHeader("Accept-Ranges") + "\r\n";
    head += "Connection: close\r\n";
    head += "Access-Control-Allow-Origin: *\r\n";
    head += "Cache-Control: no-store\r\n\r\n";
    m_socket->write(head);
    if (m_head) {
        m_done = true;
        m_reply->abort();
    }
}

void ShareServer::onNewConnection() {
    while (m_tcp && m_tcp->hasPendingConnections()) {
        QTcpSocket *socket = m_tcp->nextPendingConnection();
        auto *conn = new ShareConnection(socket, this);
        conn->setParent(m_tcp);
    }
}

ShareServer::ShareServer(QObject *parent)
    : QObject(parent), m_nam(new QNetworkAccessManager(this)) {}

ShareServer::~ShareServer() {
    stop();
}

bool ShareServer::start(const QString &bindAddress, int port,
                        const QString &thumbCacheDir, QString *error) {
    stop();
    m_tcp = new QTcpServer(this);
    QHostAddress addr;
    const QString bind = bindAddress.trimmed();
    if (bind.isEmpty() || bind == QStringLiteral("0.0.0.0") ||
        bind == QStringLiteral("*")) {
        addr = QHostAddress::AnyIPv4;
    } else if (bind == QStringLiteral("::")) {
        addr = QHostAddress::AnyIPv6;
    } else if (bind.compare(QStringLiteral("localhost"),
                            Qt::CaseInsensitive) == 0) {
        addr = QHostAddress::LocalHost;
    } else if (!addr.setAddress(bind)) {
        if (error)
            *error = tr("Invalid bind address: %1").arg(bind);
        m_tcp->deleteLater();
        m_tcp = nullptr;
        return false;
    }
    if (!m_tcp->listen(addr, static_cast<quint16>(qBound(0, port, 65535)))) {
        if (error)
            *error = m_tcp->errorString();
        m_tcp->deleteLater();
        m_tcp = nullptr;
        return false;
    }
    m_port = m_tcp->serverPort();
    m_bind = bind;
    m_thumbDir = thumbCacheDir;
    if (!m_thumbDir.isEmpty())
        QDir().mkpath(m_thumbDir);
    connect(m_tcp, &QTcpServer::newConnection, this,
            [this] { onNewConnection(); });
    return true;
}

void ShareServer::stop() {
    if (m_tcp) {
        m_tcp->close();
        m_tcp->deleteLater();
        m_tcp = nullptr;
    }
    m_port = 0;
}

bool ShareServer::isRunning() const {
    return m_tcp && m_tcp->isListening();
}

int ShareServer::postCount() const {
    return m_catalog ? m_catalog->size() : 0;
}

QStringList ShareServer::urls() const {
    QStringList out;
    if (!isRunning())
        return out;
    const QString bind = m_bind.trimmed();
    if (bind.compare(QStringLiteral("localhost"), Qt::CaseInsensitive) == 0) {
        out << QStringLiteral("http://127.0.0.1:%1").arg(m_port);
        return out;
    }
    if (!bind.isEmpty() && bind != QStringLiteral("0.0.0.0") &&
        bind != QStringLiteral("*") && bind != QStringLiteral("::")) {
        QHostAddress a;
        if (a.setAddress(bind)) {
            if (a.isLoopback())
                out << QStringLiteral("http://127.0.0.1:%1").arg(m_port);
            else
                out << QStringLiteral("http://%1:%2").arg(a.toString()).arg(m_port);
            return out;
        }
    }
    for (const QHostAddress &a : QNetworkInterface::allAddresses()) {
        if (a.protocol() != QAbstractSocket::IPv4Protocol)
            continue;
        if (a.isLoopback() || a.isNull())
            continue;
        out << QStringLiteral("http://%1:%2").arg(a.toString()).arg(m_port);
    }
    std::sort(out.begin(), out.end());
    out << QStringLiteral("http://127.0.0.1:%1").arg(m_port);
    return out;
}

void ShareServer::setCatalog(std::shared_ptr<const ShareCatalog> catalog) {
    m_catalog = std::move(catalog);
}

void ShareServer::setMediaResolver(ResolveMedia resolver) {
    m_resolve = std::move(resolver);
}

void ShareServer::setRequestDecorator(RequestDecorator decorator) {
    m_decorate = std::move(decorator);
}

void ShareServer::setNetworkProxy(const QNetworkProxy &proxy) {
    if (m_nam)
        m_nam->setProxy(proxy);
}
