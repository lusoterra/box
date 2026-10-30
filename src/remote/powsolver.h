#pragma once

// Native proof-of-work for API-gated hosts (soybooru: HTTP 449
// {"error":"pow_required"}). The clearance cookie binds to the solving
// egress IP (verified live: same-IP calls pass, any other IP 449s), so a
// browser-solved token dies on every egress change (Tor exit rotation
// especially) and re-capture can never converge. PowSolver instead runs
// the site's own protocol - GET /api/pow/challenge, SHA-256 prefix
// search (a port of its pow-worker), POST /api/pow/verify - on the
// caller's QNAM (same proxy, same egress, same cookie jar): the token
// lands in that jar and the retried request just works. No browser
// involved; captchas still need a human.
#include <QObject>
#include <QString>
#include <QStringList>

#include <functional>

class QNetworkAccessManager;

namespace RemotePosts {
// Hex nonce with SHA-256(challenge + nonce) carrying at least difficulty
// leading zero bits ("" + *ok=false when over budget or on a bad
// difficulty). Pure (unit-tested).
QString solvePowWork(const QString &challenge, int difficulty,
                     qint64 budget, bool *ok);
} // namespace RemotePosts

// One-shot solver: challenge fetch + verify POST both ride nam, then the
// jar's fresh cookies for baseUrl come back via done(ok, freshCookies,
// error). GUI thread for the QNAM hops; hashing rides QtConcurrent.
class PowSolver : public QObject {
    Q_OBJECT
public:
    static void solve(QNetworkAccessManager *nam, const QString &baseUrl,
                      QObject *context,
                      std::function<void(bool, const QStringList &,
                                         const QString &)>
                          done);
};
