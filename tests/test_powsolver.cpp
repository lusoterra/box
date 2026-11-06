// Native PoW worker math: leading-zero-bit search over
// SHA-256(challenge + hex(nonce)). Unit-only (no network; the live
// protocol shape was verified out-of-band against soybooru).
#include "powsolver.h"

#include <QCryptographicHash>
#include <QCoreApplication>
#include <QtTest>

namespace {
bool hasLeadingZeroBits(const QString &challenge, const QString &nonceHex,
                        int difficulty) {
    const QByteArray digest = QCryptographicHash::hash(
        challenge.toUtf8() + nonceHex.toUtf8(),
        QCryptographicHash::Sha256);
    const int full = difficulty / 8;
    const int bits = difficulty % 8;
    for (int i = 0; i < full && i < digest.size(); ++i) {
        if (static_cast<quint8>(digest.at(i)) != 0)
            return false;
    }
    if (bits > 0 && full < digest.size()) {
        const quint8 mask = static_cast<quint8>(0xFFu << (8 - bits));
        if ((static_cast<quint8>(digest.at(full)) & mask) != 0)
            return false;
    }
    return true;
}
} // namespace

class TestPowSolver : public QObject {
    Q_OBJECT
private slots:
    void findsValidNonce() {
        bool ok = false;
        const QString nonce = RemotePosts::solvePowWork(
            QStringLiteral("soybooru-test-challenge"), 10, 100000, &ok);
        QVERIFY(ok);
        QVERIFY(!nonce.isEmpty());
        QVERIFY(hasLeadingZeroBits(
            QStringLiteral("soybooru-test-challenge"), nonce, 10));
    }

    void rejectsGarbage() {
        bool ok = true;
        QVERIFY(RemotePosts::solvePowWork(QString(), 8, 1000, &ok).isEmpty());
        QVERIFY(!ok);
        QVERIFY(RemotePosts::solvePowWork(QStringLiteral("x"), -1, 1000, &ok)
                    .isEmpty());
        QVERIFY(!ok);
        QVERIFY(RemotePosts::solvePowWork(QStringLiteral("x"), 65, 1000, &ok)
                    .isEmpty());
        QVERIFY(!ok);
        QVERIFY(RemotePosts::solvePowWork(QStringLiteral("x"), 8, 0, &ok)
                    .isEmpty());
        QVERIFY(!ok);
    }

    void respectsBudget() {
        // Difficulty 24 needs ~16M hashes on average: a 100-hash budget
        // must fail fast instead of hanging the suite.
        bool ok = true;
        QVERIFY(RemotePosts::solvePowWork(QStringLiteral("x"), 24, 100, &ok)
                    .isEmpty());
        QVERIFY(!ok);
    }

    void solvePowGaps() {
        // src/remote/powsolver.h:25
        // difficulty 0 -> any nonce works (first try, empty nonce? no, nonce must be hex)
        bool ok = false;
        const QString n = RemotePosts::solvePowWork(QStringLiteral("challenge"), 0, 10, &ok);
        QVERIFY(ok);
        QVERIFY(!n.isEmpty());
        // bad difficulty (negative) -> ok false
        bool ok2 = true;
        const QString n2 = RemotePosts::solvePowWork(QStringLiteral("challenge"), -1, 10, &ok2);
        QVERIFY(n2.isEmpty());
        QVERIFY(!ok2);
        // null ok pointer -> no crash
        const QString n3 = RemotePosts::solvePowWork(QStringLiteral("challenge"), 1, 10, nullptr);
        QVERIFY(!n3.isEmpty());
    }
};

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    TestPowSolver tc;
    return QTest::qExec(&tc, argc, argv);
}
#include "test_powsolver.moc"
