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
#include <QUrl>
#include <functional>

namespace {
QString extOf(const QString &url, const QString &hint) {
    const QString u = url.trimmed().toLower();
    const QString h = hint.trimmed().toLower();
    auto extFrom = [](const QString &s) {
        const QString clean = s.split(u'?').first();
        const int dot = clean.lastIndexOf(u'.');
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

QNetworkAccessManager *namFor(const QString &url) {
    static QNetworkAccessManager nam;
    return &nam;
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
    if (url.trimmed().isEmpty() || !onReady) {
        if (onReady)
            onReady(QString());
        return;
    }
    const QString path = tempPathFor(url, extHint);
    if (QFile::exists(path) && QFileInfo(path).size() > 0) {
        onReady(path);
        return;
    }
    QNetworkRequest req{QUrl(url)};
    req.setTransferTimeout(30000);
    req.setHeader(QNetworkRequest::UserAgentHeader,
                  QStringLiteral("box/0.1 (external media)"));
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply *reply = namFor(url)->get(req);
    QObject::connect(reply, &QNetworkReply::finished, reply, [reply, path, onReady]() {
        if (reply->error() == QNetworkReply::NoError) {
            QFile f(path + QStringLiteral(".part"));
            if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                f.write(reply->readAll());
                f.close();
                QFile::remove(path);
                QFile::rename(path + QStringLiteral(".part"), path);
            }
        }
        const QString result =
            (QFile::exists(path) && QFileInfo(path).size() > 0)
                ? path
                : QString();
        reply->deleteLater();
        onReady(result);
    });
}