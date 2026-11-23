#include "hydrus.h"

#include <QFile>
#include <QTemporaryDir>
#include <QTest>
#include <QUrlQuery>

class TestHydrus : public QObject {
    Q_OBJECT
private slots:
    void metadataFound() {
        const QByteArray body = R"({
            "hydrus_version": 696,
            "metadata": [{
                "file_id": 123,
                "hash": "abcdef",
                "tags": {
                    "repo": {
                        "name": "public tag repository",
                        "storage_tags": {"0": ["creator:alice", "beach"]},
                        "display_tags": {"0": ["creator:alice", "beach", "landform"]}
                    },
                    "all known tags": {
                        "storage_tags": {"0": ["beach", "character:bob"]}
                    }
                }
            }]
        })";
        const Hydrus::FileTags f = Hydrus::parseMetadata(body, "abcdef");
        QVERIFY(f.ok);
        QVERIFY(f.found);
        QCOMPARE(f.fileId, qint64(123));
        QCOMPARE(f.hash, QStringLiteral("abcdef"));
        // Storage tags only (landform is a parent), deduped across services.
        QStringList got = f.tags;
        QStringList want{QStringLiteral("creator:alice"),
                         QStringLiteral("beach"),
                         QStringLiteral("character:bob")};
        got.sort();
        want.sort();
        QCOMPARE(got, want);
    }

    void metadataMissing() {
        const QByteArray body = R"({"metadata": [
            {"file_id": null, "hash": "001122"}
        ]})";
        const Hydrus::FileTags f = Hydrus::parseMetadata(body, "001122");
        QVERIFY(f.ok);
        QVERIFY(!f.found);
        QCOMPARE(f.hash, QStringLiteral("001122"));
        QVERIFY(f.tags.isEmpty());
    }

    void metadataPrefersWantedHash() {
        const QByteArray body = R"({"metadata": [
            {"file_id": 1, "hash": "aaaa", "tags": {}},
            {"file_id": 2, "hash": "bbbb", "tags": {}}
        ]})";
        const Hydrus::FileTags f = Hydrus::parseMetadata(body, "BBBB");
        QVERIFY(f.found);
        QCOMPARE(f.fileId, qint64(2));
    }

    void metadataRejectsGarbage() {
        const Hydrus::FileTags f =
            Hydrus::parseMetadata(QByteArrayLiteral("<html>nope</html>"),
                                  QString());
        QVERIFY(!f.ok);
        QVERIFY(!f.error.isEmpty());
    }

    void namespaceBuckets() {
        const Hydrus::CategorizedTags c = Hydrus::categorize(
            {QStringLiteral("creator:tetsu (teppei)"),
             QStringLiteral("character:kouki (pokemon)"),
             QStringLiteral("series:pokémon"), QStringLiteral("studio:nintendo"),
             QStringLiteral("meta:bad id"), QStringLiteral("species:pokémon"),
             QStringLiteral("rating:safe"), QStringLiteral("plain_tag")});
        QCOMPARE(c.artist, QStringList{QStringLiteral("tetsu (teppei)")});
        QCOMPARE(c.character, QStringList{QStringLiteral("kouki (pokemon)")});
        QCOMPARE(c.copyright,
                 (QStringList{QStringLiteral("pokémon"),
                              QStringLiteral("nintendo")}));
        QCOMPARE(c.meta, QStringList{QStringLiteral("bad id")});
        QCOMPARE(c.general,
                 (QStringList{QStringLiteral("species:pokémon"),
                              QStringLiteral("rating:safe"),
                              QStringLiteral("plain_tag")}));
        QCOMPARE(c.rating, QStringLiteral("safe"));
    }

    void categorizeDedupes() {
        const Hydrus::CategorizedTags c = Hydrus::categorize(
            {QStringLiteral("creator:alice"), QStringLiteral("creator:alice"),
             QStringLiteral("beach"), QStringLiteral("beach")});
        QCOMPARE(c.artist, QStringList{QStringLiteral("alice")});
        QCOMPARE(c.general, QStringList{QStringLiteral("beach")});
    }

    void lookupUrl() {
        const QUrl u =
            Hydrus::metadataUrl(QStringLiteral("127.0.0.1:45869/"),
                                QStringLiteral("abc"));
        QCOMPARE(u.scheme(), QStringLiteral("http"));
        QCOMPARE(u.path(), QStringLiteral("/get_files/file_metadata"));
        const QUrlQuery q(u);
        QCOMPARE(q.queryItemValue(QStringLiteral("hash")), QStringLiteral("abc"));
        QCOMPARE(q.queryItemValue(QStringLiteral("include_services_object")),
                 QStringLiteral("false"));
    }

    void verifyText() {
        QCOMPARE(Hydrus::verifySummary(
                     R"({"name":"hydd","hydrus_version":696})"),
                 QStringLiteral("hydd (Hydrus 696)"));
        QCOMPARE(Hydrus::verifySummary(QByteArrayLiteral("garbage")),
                 QStringLiteral("Connected (unreadable version reply)"));
    }

    void fileHashPass() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("blob.bin"));
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(QByteArrayLiteral("abc"));
        f.close();
        const FileContentHashes h = fileContentHashes(path);
        QVERIFY(h.ok);
        QCOMPARE(h.md5, QStringLiteral("900150983cd24fb0d6963f7d28e17f72"));
        QCOMPARE(h.sha1, QStringLiteral("a9993e364706816aba3e25717850c26c9cd0d89d"));
        QCOMPARE(h.sha256,
                 QStringLiteral("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
        QCOMPARE(Hydrus::hashFor(Hydrus::HashKind::Md5, h), h.md5);
        QCOMPARE(Hydrus::hashFor(Hydrus::HashKind::Sha1, h), h.sha1);
        QCOMPARE(Hydrus::hashFor(Hydrus::HashKind::Sha256, h), h.sha256);
    }

    void fileHashUnreadable() {
        QTemporaryDir dir;
        const FileContentHashes h =
            fileContentHashes(dir.filePath(QStringLiteral("nope")));
        QVERIFY(!h.ok);
        QVERIFY(h.sha256.isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestHydrus)
#include "test_hydrus.moc"
