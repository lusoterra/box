#include "powsolver.h"

#include "remotesource.h"
#include "sitequirks.h"

#include <QCryptographicHash>
#include <QFutureWatcher>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkCookieJar>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QtConcurrent>

namespace RemotePosts {
QString solvePowWork(const QString &challenge, int difficulty,
                     qint64 budget, bool *ok) {
    if (ok)
        *ok = false;
    if (challenge.isEmpty() || difficulty < 0 || difficulty > 64 ||
        budget <= 0)
        return {};
    const int full = difficulty / 8;
    const int bits = difficulty % 8;
    const quint8 mask =
        bits == 0 ? 0 : static_cast<quint8>(0xFFu << (8 - bits));
    const QByteArray prefix = challenge.toUtf8();
    for (qint64 n = 0; n < budget; ++n) {
        const QByteArray digest = QCryptographicHash::hash(
            prefix + QByteArray::number(n, 16), QCryptographicHash::Sha256);
        bool good = true;
        for (int i = 0; i < full && i < digest.size(); ++i) {
            if (static_cast<quint8>(digest.at(i)) != 0) {
                good = false;
                break;
            }
        }
        if (good && bits > 0 && full < digest.size()) {
            if ((static_cast<quint8>(digest.at(full)) & mask) != 0)
                good = false;
        }
        if (good) {
            if (ok)
                *ok = true;
            return QString::number(n, 16);
        }
    }
    return {};
}
} // namespace RemotePosts

namespace {
// One attempt: sequential QNAM hops on the caller's stack, hashing off
// the GUI thread. Deleted on completion.
class PowAttempt : public QObject {
    Q_OBJECT
public:
    PowAttempt(QNetworkAccessManager *nam, const QString &baseUrl,
               std::function<void(bool, const QStringList &, const QString &)>
                   done)
        : m_nam(nam), m_base(baseUrl), m_done(std::move(done)) {
        if (!m_nam->cookieJar())
            m_nam->setCookieJar(new QNetworkCookieJar(m_nam));
    }
    void start() {
        // The challenge id embeds a timestamp (likely short-lived):
        // the whole attempt gets 120 seconds, then it reports failure.
        m_deadline = new QTimer(this);
        m_deadline->setSingleShot(true);
        connect(m_deadline, &QTimer::timeout, this, [this] {
            finish(false, {}, tr("Proof-of-work timed out"));
        });
        m_deadline->start(120000);
        QNetworkRequest req(QUrl(m_base + QStringLiteral("/api/pow/challenge")));
        req.setTransferTimeout(30000);
        RemoteSource::applyCookies(req);
        req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
        QNetworkReply *reply = m_nam->get(req);
        connect(reply, &QNetworkReply::finished, this,
                [this, reply] { onChallenge(reply); });
    }

private:
    void finish(bool ok, const QStringList &fresh, const QString &error) {
        if (m_finished)
            return;
        m_finished = true;
        if (m_deadline)
            m_deadline->stop();
        auto done = std::move(m_done);
        deleteLater();
        if (done)
            done(ok, fresh, error);
    }
    void onChallenge(QNetworkReply *reply) {
        reply->deleteLater();
        if (m_finished)
            return;
        if (reply->error() != QNetworkReply::NoError) {
            finish(false, {}, tr("PoW challenge fetch failed: %1")
                                      .arg(reply->errorString()));
            return;
        }
        const QJsonObject o =
            QJsonDocument::fromJson(reply->readAll()).object();
        const QString id = o.value(QStringLiteral("id")).toString();
        const QString challenge =
            o.value(QStringLiteral("challenge")).toString();
        const int difficulty = o.value(QStringLiteral("difficulty")).toInt(-1);
        if (id.isEmpty() || challenge.isEmpty() || difficulty < 0) {
            finish(false, {}, tr("PoW challenge was unparseable"));
            return;
        }
        // Expected work is 2^difficulty hashes: refuse absurd budgets
        // (a cranked difficulty wants the multi-core browser, not us).
        static constexpr qint64 kMaxHashes = 64000000;
        qint64 budget = difficulty >= 30
                            ? kMaxHashes + 1
                            : qMin<qint64>(4LL << difficulty, kMaxHashes);
        if (budget > kMaxHashes) {
            finish(false, {},
                   tr("PoW difficulty %1 is too high to solve in-app "
                      "(solve it in the capture browser instead)")
                       .arg(difficulty));
            return;
        }
        auto *watcher = new QFutureWatcher<QString>(this);
        connect(watcher, &QFutureWatcher<QString>::finished, this,
                [this, watcher, id, challenge] {
                    watcher->deleteLater();
                    if (m_finished)
                        return;
                    const QString nonce = watcher->result();
                    if (nonce.isEmpty()) {
                        finish(false, {},
                               tr("PoW solution not found in budget"));
                        return;
                    }
                    verify(id, challenge, nonce);
                });
        watcher->setFuture(QtConcurrent::run(
            [challenge, difficulty, budget] {
                bool ok = false;
                const QString nonce = RemotePosts::solvePowWork(
                    challenge, difficulty, budget, &ok);
                return ok ? nonce : QString();
            }));
    }
    void verify(const QString &id, const QString &challenge,
                const QString &nonce) {
        QJsonObject body;
        body[QStringLiteral("challengeId")] = id;
        body[QStringLiteral("challenge")] = challenge;
        body[QStringLiteral("nonce")] = nonce;
        QNetworkRequest req(QUrl(m_base + QStringLiteral("/api/pow/verify")));
        req.setTransferTimeout(30000);
        req.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/json"));
        RemoteSource::applyCookies(req);
        req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
        QNetworkReply *reply = m_nam->post(
            req, QJsonDocument(body).toJson(QJsonDocument::Compact));
        connect(reply, &QNetworkReply::finished, this,
                [this, reply] {
                    reply->deleteLater();
                    if (m_finished)
                        return;
                    if (reply->error() != QNetworkReply::NoError) {
                        finish(false, {},
                               tr("PoW verify failed: %1")
                                   .arg(reply->errorString()));
                        return;
                    }
                    const bool ok = QJsonDocument::fromJson(reply->readAll())
                                        .object()
                                        .value(QStringLiteral("success"))
                                        .toBool();
                    if (!ok) {
                        finish(false, {},
                               tr("PoW verify rejected the solution"));
                        return;
                    }
                    const QString host =
                        QUrl(m_base).host().toLower();
                    const QList<QNetworkCookie> jar =
                        m_nam->cookieJar()
                            ? m_nam->cookieJar()->cookiesForUrl(QUrl(m_base))
                            : QList<QNetworkCookie>();
                    finish(true,
                           RemotePosts::harvestCookies(jar, host),
                           QString());
                });
    }

    QNetworkAccessManager *m_nam;
    QString m_base;
    std::function<void(bool, const QStringList &, const QString &)> m_done;
    QTimer *m_deadline = nullptr;
    bool m_finished = false;
};
} // namespace

void PowSolver::solve(
    QNetworkAccessManager *nam, const QString &baseUrl, QObject *context,
    std::function<void(bool, const QStringList &, const QString &)> done) {
    if (!nam || baseUrl.isEmpty()) {
        if (done)
            done(false, {}, QObject::tr("PoW solver has no network stack"));
        return;
    }
    auto *attempt = new PowAttempt(nam, baseUrl, std::move(done));
    if (context)
        attempt->connect(context, &QObject::destroyed, attempt,
                         &QObject::deleteLater);
    attempt->start();
}

#include "powsolver.moc"
