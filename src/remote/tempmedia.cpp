#include "tempmedia.h"

#include "remotesource.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStandardPaths>
#include <QString>
#include <QTemporaryFile>
#include <QUrl>
#include <functional>

namespace {
QString extOf(const QString &url, const QString &hint) {
    const QString u = url.trimmed().toLower();
    const QString h = hint.trimmed().toLower();
    auto extFrom = [](const QString &s) {
        const QString clean = s.split(u'?').first();
        const qsizetype dot = clean.lastIndexOf(u'.');
        if (dot < 0)
            return QString();
        QString e = clean.mid(dot + 1);
        if (e.size() < 1 || e.size() > 5)
            return QString();
        for (const QChar &c : e)
            if (!c.isLetter())
                return QString();
        return e;
    };
    const QString fromUrl = extFrom(u);
    if (!fromUrl.isEmpty())
        return fromUrl;
    const QString fromHint = extFrom(h);
    if (!fromHint.isEmpty())
        return fromHint;
    return QStringLiteral("bin");
}

QString tempPathFor(const QString &url, const QString &extHint) {
    const QByteArray hash = QCryptographicHash::hash(url.toUtf8(),
                                                    QCryptographicHash::Sha1)
                               .toHex();
    return TempMedia::tempDir() + u'/' + QString::fromLatin1(hash) + u'.' +
           extOf(url, extHint);
}

QNetworkAccessManager *namFor() {
    static QNetworkAccessManager nam;
    return &nam;
}

bool contentTypeMedia(const QString &ct) {
    const QString c = ct.trimmed().toLower();
    return c.startsWith(QStringLiteral("image/")) ||
           c.startsWith(QStringLiteral("video/")) ||
           c.startsWith(QStringLiteral("application/octet-stream")) ||
           c.startsWith(QStringLiteral("binary/octet-stream"));
}

bool wallStatus(int httpStatus) {
    return httpStatus == 401 || httpStatus == 403 || httpStatus == 429 ||
           httpStatus == 449 || httpStatus == 503;
}

bool fileStillMedia(const QString &path) {
    if (!QFile::exists(path) || QFileInfo(path).size() <= 0)
        return false;
    if (path.endsWith(QStringLiteral(".svg"), Qt::CaseInsensitive))
        return true; // XML text: no magic bytes to sniff
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return false;
    const QByteArray head = f.read(512);
    return RemotePosts::looksLikeMediaBytes(head);
}
} // namespace

QString TempMedia::tempDir() {
    const QString d = QStandardPaths::writableLocation(
                          QStandardPaths::TempLocation) +
                      QStringLiteral("/box-ext");
    QDir().mkpath(d);
    return d;
}

void TempMedia::ensureTemp(const QString &url, const QString &extHint,
                          const std::function<void(const QString &)> &onReady) {
    if (!onReady)
        return;
    ensureTemp(url, extHint,
               [onReady](const Result &r) { onReady(r.path); });
}

void TempMedia::ensureTemp(const QString &url, const QString &extHint,
                          const std::function<void(const Result &)> &onReady) {
    if (url.trimmed().isEmpty() || !onReady) {
        if (onReady) {
            Result r;
            r.error = QObject::tr("Empty URL");
            onReady(r);
        }
        return;
    }
    const QString path = tempPathFor(url, extHint);
    // Validate the cached copy too: older builds happily saved wall HTML
    // under a .jpg name, and reusing it would keep "opening the JSON".
    if (fileStillMedia(path)) {
        Result r;
        r.path = path;
        onReady(r);
        return;
    }
    QFile::remove(path);
    QNetworkRequest req{QUrl(url)};
    req.setTransferTimeout(30000);
    req.setHeader(QNetworkRequest::UserAgentHeader,
                  QStringLiteral("box/0.1 (external media)"));
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply *reply = namFor()->get(req);
    QObject::connect(reply, &QNetworkReply::finished, reply,
                     [reply, path, onReady]() {
        Result r;
        const int httpStatus = reply->attribute(
                                   QNetworkRequest::HttpStatusCodeAttribute)
                                   .toInt();
        const QString contentType =
            reply->header(QNetworkRequest::ContentTypeHeader).toString();
        const QByteArray body = reply->readAll();
        const bool challenge =
            RemotePosts::looksLikeCloudflareWall(body) ||
            RemotePosts::looksLikeChallengePage(body);
        r.walled = wallStatus(httpStatus) || challenge ||
                   contentType.trimmed().toLower().startsWith(
                       QStringLiteral("text/"));
        if (reply->error() != QNetworkReply::NoError || httpStatus >= 400) {
            r.walled = r.walled || wallStatus(httpStatus) || challenge;
            r.error = reply->error() != QNetworkReply::NoError
                          ? reply->errorString()
                          : QObject::tr("HTTP %1").arg(httpStatus);
        } else if (!contentTypeMedia(contentType) &&
                   !RemotePosts::looksLikeMediaBytes(body.left(512))) {
            // HTML/JSON wall or login page: never stage it as media.
            r.walled = true;
            r.error = QObject::tr("Server returned a page, not media");
        } else {
            r.walled = false; // bytes sniffed as real media
            // Staged write: a pre-planted symlink at path/.part (shared
            // /tmp dir, predictable sha1 name) must never redirect the
            // download bytes elsewhere. QTemporaryFile is O_EXCL-created
            // and rename() replaces a link instead of following it.
            QTemporaryFile staged(QFileInfo(path).absolutePath() +
                                  QStringLiteral("/.box-part-XXXXXX"));
            staged.setAutoRemove(false);
            if (staged.open()) {
                staged.write(body);
                const QString stagedName = staged.fileName();
                staged.close();
                QFile::remove(path);
                if (QFile::rename(stagedName, path))
                    r.path = path;
                else
                    QFile::remove(stagedName);
            }
            if (r.path.isEmpty())
                r.error = QObject::tr("Cannot write %1").arg(path);
        }
        if (!r.path.isEmpty() && !fileStillMedia(r.path)) {
            QFile::remove(r.path);
            r.path.clear();
            r.error = QObject::tr("Downloaded file is not media");
            r.walled = true;
        }
        reply->deleteLater();
        onReady(r);
    });
}
