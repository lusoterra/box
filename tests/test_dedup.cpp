// Pixel hashing + duplicate grouping: pure unit coverage. The scanner
// and dialog are exercised through the regular GUI build (and the full
// ctest suite); here we pin the hash stability and the family/type
// isolation rules.
#include "dedupdb.h"
#include "dedupgroups.h"
#include "dedupscanner.h"
#include "phash.h"

#include <QDir>
#include <QFile>
#include <QImage>
#include <QPainter>
#include <QTemporaryDir>
#include <QtTest>

namespace {

QImage patternImage(int size) {
    QImage img(size, size, QImage::Format_RGB32);
    QPainter p(&img);
    QLinearGradient g(0, 0, size, size);
    g.setColorAt(0.0, QColor(20, 40, 90));
    g.setColorAt(0.5, QColor(210, 120, 60));
    g.setColorAt(1.0, QColor(30, 160, 120));
    p.fillRect(img.rect(), g);
    p.setBrush(QColor(240, 240, 235));
    p.setPen(Qt::NoPen);
    p.drawEllipse(QPointF(size * 0.32, size * 0.35), size * 0.18,
                  size * 0.18);
    p.setBrush(QColor(15, 15, 20));
    p.drawRect(QRectF(size * 0.58, size * 0.55, size * 0.28, size * 0.2));
    p.end();
    return img;
}

QImage otherImage(int size) {
    QImage img(size, size, QImage::Format_RGB32);
    img.fill(QColor(245, 235, 210));
    QPainter p(&img);
    p.setPen(QPen(QColor(20, 20, 20), qMax(1, size / 40)));
    for (int y = 0; y < size; y += size / 8)
        p.drawLine(0, y, size, size - y);
    p.end();
    return img;
}

DedupFile row(const QString &scope, const QString &kind, const QString &path,
              const QString &sha, quint64 phash, bool hasPhash = true) {
    DedupFile f;
    f.scope = scope;
    f.kind = kind;
    f.root = QStringLiteral("/tmp");
    f.path = path;
    f.size = 1000;
    f.sha256 = sha;
    f.phash = phash;
    f.hasPhash = hasPhash;
    f.width = 100;
    f.height = 100;
    return f;
}

} // namespace

class TestDedup : public QObject {
    Q_OBJECT
private slots:
    void hashIsStableAndScaleTolerant() {
        const QImage big = patternImage(160);
        const QImage small =
            big.scaled(64, 64, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        const quint64 a = perceptualHashImage(big);
        const quint64 b = perceptualHashImage(small);
        QVERIFY(a != 0);
        QVERIFY(b != 0);
        QVERIFY2(phashDistance(a, b) <= 6,
                 qPrintable(QStringLiteral("distance %1")
                                .arg(phashDistance(a, b))));
        QCOMPARE(perceptualHashImage(big), a);
    }

    void hashSeparatesDifferentPictures() {
        const quint64 a = perceptualHashImage(patternImage(128));
        const quint64 b = perceptualHashImage(otherImage(128));
        QVERIFY(a != 0);
        QVERIFY(b != 0);
        QVERIFY2(phashDistance(a, b) > 12,
                 qPrintable(QStringLiteral("distance %1")
                                .arg(phashDistance(a, b))));
    }

    void hashSurvivesJpegReencode() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QImage img = patternImage(192);
        const QString png = dir.filePath(QStringLiteral("a.png"));
        const QString jpg = dir.filePath(QStringLiteral("b.jpg"));
        QVERIFY(img.save(png));
        QVERIFY(img.save(jpg, "JPG", 82));
        const PixelHash a = perceptualHashFile(png);
        const PixelHash b = perceptualHashFile(jpg);
        QVERIFY(a.ok);
        QVERIFY(b.ok);
        QVERIFY2(phashDistance(a.hash, b.hash) <= 6,
                 qPrintable(QStringLiteral("distance %1")
                                .arg(phashDistance(a.hash, b.hash))));
    }

    void groupsArePerFamilyAndType() {
        const quint64 base = 0x0123456789abcdefULL;
        QVector<DedupFile> rows;
        rows << row(QStringLiteral("archive"), QStringLiteral("image"),
                    QStringLiteral("/a/1.png"), QStringLiteral("sha-a"), base);
        rows << row(QStringLiteral("archive"), QStringLiteral("image"),
                    QStringLiteral("/a/2.png"), QStringLiteral("sha-b"), base);
        rows << row(QStringLiteral("download"), QStringLiteral("image"),
                    QStringLiteral("/d/1.png"), QStringLiteral("sha-c"), base);
        rows << row(QStringLiteral("archive"), QStringLiteral("animated"),
                    QStringLiteral("/a/3.gif"), QStringLiteral("sha-d"), base);
        // Exact-byte copy, no perceptual hash (e.g. an unreadable image):
        rows << row(QStringLiteral("archive"), QStringLiteral("image"),
                    QStringLiteral("/a/4.png"), QStringLiteral("sha-a"), 0,
                    false);
        // Same family/type, far enough away to stay alone.
        rows << row(QStringLiteral("archive"), QStringLiteral("image"),
                    QStringLiteral("/a/5.png"), QStringLiteral("sha-e"),
                    base ^ 0xffULL);

        const QVector<DedupGroup> groups = buildDedupGroups(rows, 6);
        QCOMPARE(groups.size(), 1);
        QCOMPARE(groups.first().scope, QStringLiteral("archive"));
        QCOMPARE(groups.first().kind, QStringLiteral("image"));
        QCOMPARE(groups.first().files.size(), 3);
    }

    void groupNeedsMatchesWithinOneFamily() {
        QVector<DedupFile> rows;
        rows << row(QStringLiteral("archive"), QStringLiteral("image"),
                    QStringLiteral("/a.png"), QStringLiteral("same"), 0,
                    false);
        rows << row(QStringLiteral("download"), QStringLiteral("image"),
                    QStringLiteral("/d.png"), QStringLiteral("same"), 0,
                    false);
        QVERIFY(buildDedupGroups(rows, 6).isEmpty());
    }

    void scannerCachesAndPrunes() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString root = dir.filePath(QStringLiteral("archive"));
        QVERIFY(QDir().mkpath(root));
        const QImage img = patternImage(128);
        QVERIFY(img.save(root + QStringLiteral("/a.png")));
        QVERIFY(img.scaled(96, 96, Qt::KeepAspectRatio,
                           Qt::SmoothTransformation)
                    .save(root + QStringLiteral("/b.jpg"), "JPG", 82));
        QVERIFY(otherImage(128).save(root + QStringLiteral("/c.png")));

        DedupScanOptions opts;
        opts.dbPath = dir.filePath(QStringLiteral("dedup.db"));
        opts.sources = {{QStringLiteral("archive"), root}};

        DedupScanTask first;
        first.run(opts);
        QVERIFY2(first.result.error.isEmpty(),
                 qPrintable(first.result.error));
        QCOMPARE(first.result.total, 3);
        QCOMPARE(first.result.hashed, 3);

        {
            DedupDb db;
            QVERIFY(db.open(opts.dbPath));
            const QVector<DedupFile> rows = db.files(QStringLiteral("archive"));
            QCOMPARE(rows.size(), 3);
            const QVector<DedupGroup> groups = buildDedupGroups(rows, 6);
            QVERIFY2(!groups.isEmpty(), "the resized JPEG should match the PNG");
        }

        DedupScanTask second;
        second.run(opts);
        QCOMPARE(second.result.hashed, 0);
        QCOMPARE(second.result.cached, 3);

        QVERIFY(QFile::remove(root + QStringLiteral("/b.jpg")));
        DedupScanTask third;
        third.run(opts);
        QCOMPARE(third.result.total, 2);
        DedupDb db;
        QVERIFY(db.open(opts.dbPath));
        QCOMPARE(db.files(QStringLiteral("archive")).size(), 2);
    }

    void databaseCachesAndPrunes() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("dedup.db"));
        {
            DedupDb db;
            QVERIFY(db.open(path));
            QVERIFY(db.begin());
            DedupFile a = row(QStringLiteral("archive"),
                              QStringLiteral("image"),
                              QStringLiteral("/a.png"), QStringLiteral("aa"),
                              1234);
            a.size = 500;
            a.mtime = 77;
            QVERIFY(db.upsert(a));
            DedupFile b = row(QStringLiteral("download"),
                              QStringLiteral("video"),
                              QStringLiteral("/b.mp4"), QStringLiteral("bb"),
                              0, false);
            QVERIFY(db.upsert(b));
            QVERIFY(db.commit());
            QCOMPARE(db.count(), 2);
            QCOMPARE(db.files(QStringLiteral("archive")).size(), 1);
            const DedupFile got = db.files(QStringLiteral("archive")).first();
            QCOMPARE(got.path, QStringLiteral("/a.png"));
            QCOMPARE(got.size, qint64(500));
            QCOMPARE(got.phash, quint64(1234));
            QVERIFY(got.hasPhash);
            db.prune(QStringLiteral("archive"), QStringLiteral("/tmp"),
                     {QStringLiteral("/gone.png")});
            QCOMPARE(db.files(QStringLiteral("archive")).size(), 0);
            QVERIFY(db.removePath(QStringLiteral("download"),
                                  QStringLiteral("/b.mp4")));
            QCOMPARE(db.count(), 0);
            db.close();
        }
        DedupDb again;
        QVERIFY(again.open(path));
        QCOMPARE(again.count(), 0);
        QVERIFY(again.clear());
    }
};

QTEST_GUILESS_MAIN(TestDedup)
#include "test_dedup.moc"
