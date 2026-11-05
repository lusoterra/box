// fileContentMd5 / isMd5Stem: content hashing for md5-less downloads.
#include "filehash.h"

#include <QCryptographicHash>
#include <QTemporaryDir>
#include <QtTest>

class TestFileHash : public QObject {
    Q_OBJECT
private slots:
    void md5KnownVector() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("abc.bin"));
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("abc");
        f.close();
        // Well-known md5("abc").
        QCOMPARE(fileContentMd5(path),
                 QStringLiteral("900150983cd24fb0d6963f7d28e17f72"));
    }

    void md5EmptyFile() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("empty.bin"));
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.close();
        QCOMPARE(fileContentMd5(path),
                 QStringLiteral("d41d8cd98f00b204e9800998ecf8427e"));
    }

    void md5MissingFile() {
        QVERIFY(fileContentMd5(QStringLiteral("/nonexistent/file.bin")).isEmpty());
    }

    void md5Largeish() {
        // Multi-chunk: larger than the 1MB read window.
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("big.bin"));
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        const QByteArray chunk(65536, 'x');
        for (int i = 0; i < 40; ++i)
            f.write(chunk);
        f.close();
        QCryptographicHash ref(QCryptographicHash::Md5);
        ref.addData(QByteArray(40 * 65536, 'x'));
        QCOMPARE(fileContentMd5(path),
                 QString::fromLatin1(ref.result().toHex()));
    }

    void stemDetection() {
        QVERIFY(isMd5Stem(QStringLiteral("Danbooru_900150983cd24fb0d6963f7d28e17f72")));
        QVERIFY(isMd5Stem(QStringLiteral("Gelbooru_D41D8CD98F00B204E9800998ECF8427E")));
        QVERIFY(!isMd5Stem(QStringLiteral("Danbooru_losn48213")));
        QVERIFY(!isMd5Stem(QStringLiteral("Safebooru_1663554")));
        QVERIFY(!isMd5Stem(QStringLiteral("plainname")));
        QVERIFY(!isMd5Stem(QStringLiteral("A_short")));
        // src/library/filehash.h: isMd5Stem requires last 32 chars hex,
        // preceded by '_' (hash at end of stem).
        QVERIFY(isMd5Stem(QStringLiteral("a_00000000000000000000000000000000"))); // min boundary
        QVERIFY(!isMd5Stem(QStringLiteral("_00000000000000000000000000000000"))); // no site prefix before '_'? size 33, at(0)=='_' ok
        QVERIFY(isMd5Stem(QStringLiteral("x_ffffffffffffffffffffffffffffffff")));
        QVERIFY(!isMd5Stem(QStringLiteral("Danbooru_900150983cd24fb0d6963f7d28e17f7z"))); // non-hex
        QVERIFY(!isMd5Stem(QStringLiteral("Danbooru-900150983cd24fb0d6963f7d28e17f72"))); // no underscore
        QVERIFY(!isMd5Stem(QStringLiteral("Danbooru_900150983cd24fb0d6963f7d28e17f72_x"))); // hash not at end
        QVERIFY(!isMd5Stem(QString()));
    }
};

QTEST_APPLESS_MAIN(TestFileHash)
#include "test_filehash.moc"
