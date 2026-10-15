// Scanner md5 fixup: sidecar-backed files without a hash stem get one
// (sidecar md5 reused, else bytes hashed), then renamed to Site_<md5>.
#include "scanner.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

namespace {
void writeFile(const QString &path, const QByteArray &data) {
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(data);
    f.close();
}

void writeSidecar(const QString &path, const QJsonObject &obj) {
    writeFile(path, QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

QJsonObject nativeSidecar(qint64 id, const QString &md5) {
    QJsonObject o;
    o[QStringLiteral("id")] = id;
    o[QStringLiteral("tag_string_general")] = QStringLiteral("sky");
    o[QStringLiteral("tag_string")] = QStringLiteral("sky");
    o[QStringLiteral("rating")] = QStringLiteral("g");
    o[QStringLiteral("file_url")] = QStringLiteral("https://example.com/f.jpg");
    o[QStringLiteral("file_ext")] = QStringLiteral("jpg");
    if (!md5.isEmpty())
        o[QStringLiteral("md5")] = md5;
    return o;
}
} // namespace

class TestScanner : public QObject {
    Q_OBJECT
private slots:
    void renamesFromSidecarMd5() {
        // _losn name + sidecar WITH md5: rename, no hashing needed.
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QDir().mkpath(dir.filePath(QStringLiteral("Site")));
        writeFile(dir.filePath(QStringLiteral("Site/Site_losn9.jpg")),
                  QByteArray("whatever-bytes"));
        writeSidecar(dir.filePath(QStringLiteral("Site/Site_losn9.json")),
                     nativeSidecar(9, QStringLiteral("d41d8cd98f00b204e9800998ecf8427e")));
        const QVector<Post> posts =
            Scanner::scan(dir.path(), nullptr, false);
        QCOMPARE(posts.size(), 1);
        QVERIFY(QFile::exists(dir.filePath(
            QStringLiteral("Site/Site_d41d8cd98f00b204e9800998ecf8427e.jpg"))));
        QVERIFY(QFile::exists(dir.filePath(
            QStringLiteral("Site/Site_d41d8cd98f00b204e9800998ecf8427e.json"))));
        QVERIFY(!QFile::exists(
            dir.filePath(QStringLiteral("Site/Site_losn9.jpg"))));
    }

    void hashesWhenSidecarLacksMd5() {
        // _losn name + sidecar WITHOUT md5: hash bytes, patch sidecar.
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QDir().mkpath(dir.filePath(QStringLiteral("Site")));
        writeFile(dir.filePath(QStringLiteral("Site/Site_losn3.jpg")),
                  QByteArray("abc"));
        writeSidecar(dir.filePath(QStringLiteral("Site/Site_losn3.json")),
                     nativeSidecar(3, QString()));
        const QVector<Post> posts =
            Scanner::scan(dir.path(), nullptr, false);
        QCOMPARE(posts.size(), 1);
        // md5("abc") below; sidecar patched with it.
        QVERIFY(QFile::exists(dir.filePath(
            QStringLiteral("Site/Site_900150983cd24fb0d6963f7d28e17f72.jpg"))));
        QFile sf(dir.filePath(QStringLiteral(
            "Site/Site_900150983cd24fb0d6963f7d28e17f72.json")));
        QVERIFY(sf.open(QIODevice::ReadOnly));
        const QJsonObject back =
            QJsonDocument::fromJson(sf.readAll()).object();
        QCOMPARE(back[QStringLiteral("md5")].toString(),
                 QStringLiteral("900150983cd24fb0d6963f7d28e17f72"));
    }

    void leavesHashedNamesAlone() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QDir().mkpath(dir.filePath(QStringLiteral("Site")));
        writeFile(dir.filePath(QStringLiteral(
                      "Site/Site_d41d8cd98f00b204e9800998ecf8427e.jpg")),
                  QByteArray("abc"));
        const QVector<Post> posts =
            Scanner::scan(dir.path(), nullptr, false);
        QCOMPARE(posts.size(), 1);
        QVERIFY(QFile::exists(dir.filePath(QStringLiteral(
            "Site/Site_d41d8cd98f00b204e9800998ecf8427e.jpg"))));
    }

    void noDuplicatesWhenFixupRenamesMidScan() {
        // Bulk _losn import: every file is renamed mid-iteration, and the
        // iterator can re-yield the new name (grid showed duplicate cells
        // pointing at the same file). One post per file, whatever the
        // directory order does.
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QDir().mkpath(dir.filePath(QStringLiteral("Site")));
        const int n = 40;
        for (int i = 0; i < n; ++i) {
            const QString base =
                QStringLiteral("Site_losn%1").arg(i);
            writeFile(dir.filePath(QStringLiteral("Site/%1.jpg").arg(base)),
                      QByteArray("bytes-") + QByteArray::number(i));
            writeSidecar(dir.filePath(QStringLiteral("Site/%1.json").arg(base)),
                         nativeSidecar(i, QString()));
        }
        const QVector<Post> posts =
            Scanner::scan(dir.path(), nullptr, false);
        QCOMPARE(posts.size(), n);
        QSet<QString> paths;
        for (const Post &p : posts)
            paths.insert(p.absFile);
        QCOMPARE(paths.size(), n);
    }
};

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    TestScanner tc;
    return QTest::qExec(&tc, argc, argv);
}
#include "test_scanner.moc"
