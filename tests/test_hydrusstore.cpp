// Isolated Hydrus tab storage: SHA-256 names, native namespace tags,
// hydd count masking, manual overrides and the record -> Post view.
#include "hydrusstore.h"

#include <QDateTime>
#include <QTemporaryDir>
#include <QtTest>

using namespace HydrusStore;

class TestHydrusStore : public QObject {
    Q_OBJECT
private slots:
    void splitsNativeNamespaces() {
        const SplitTag a = splitTag(QStringLiteral("creator:alice"));
        QCOMPARE(a.ns, QStringLiteral("creator"));
        QCOMPARE(a.tag, QStringLiteral("alice"));
        QVERIFY(a.namespaced());

        const SplitTag b = splitTag(QStringLiteral("beach"));
        QVERIFY(!b.namespaced());
        QCOMPARE(b.tag, QStringLiteral("beach"));

        // Values may contain further colons (source URLs).
        const SplitTag c = splitTag(QStringLiteral("source:https://x/y"));
        QCOMPARE(c.ns, QStringLiteral("source"));
        QCOMPARE(c.tag, QStringLiteral("https://x/y"));

        // Empty value is not a namespaced tag.
        const SplitTag d = splitTag(QStringLiteral("creator:"));
        QVERIFY(!d.namespaced());
        QCOMPARE(d.tag, QStringLiteral("creator:"));
    }

    void normalizesLikeHydrus() {
        QCOMPARE(normalizeTag(QStringLiteral("  Creator:Alice  ")),
                 QStringLiteral("creator:alice"));
        // Spaces are legal in Hydrus but not in the app's search tokens.
        QCOMPARE(normalizeTag(QStringLiteral("character:hatsune miku")),
                 QStringLiteral("character:hatsune_miku"));
        QCOMPARE(normalizeTag(QStringLiteral("ALICE's art")),
                 QStringLiteral("alice's_art"));
        QCOMPARE(normalizeTag(QStringLiteral("   ")), QString());
        QCOMPARE(normalizeTag(QStringLiteral(":foo")), QStringLiteral("foo"));
        QCOMPARE(normalizeTag(QStringLiteral("creator:")), QString());
    }

    void normalizeDedupesAndSorts() {
        const QStringList got = normalizeTags(
            {QStringLiteral("beach"), QStringLiteral("Creator:Alice"),
             QStringLiteral("creator:alice"), QStringLiteral("creator:Bob")});
        QCOMPARE(got, (QStringList{QStringLiteral("beach"),
                                   QStringLiteral("creator:alice"),
                                   QStringLiteral("creator:bob")}));
    }

    void groupsByNamespace() {
        const auto groups = groupByNamespace(
            {QStringLiteral("creator:alice"), QStringLiteral("creator:bob"),
             QStringLiteral("species:human"), QStringLiteral("beach")});
        QCOMPARE(groups.value(QStringLiteral("creator")),
                 (QStringList{QStringLiteral("alice"),
                              QStringLiteral("bob")}));
        QCOMPARE(groups.value(QStringLiteral("species")),
                 QStringList{QStringLiteral("human")});
        QCOMPARE(groups.value(QString()), QStringList{QStringLiteral("beach")});
    }

    void hashesBecomeFileNames() {
        const QString sha =
            QStringLiteral("BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD");
        QCOMPARE(fileNameForHash(sha, QStringLiteral("PNG")),
                 sha.toLower() + QStringLiteral(".png"));
        QCOMPARE(fileNameForHash(sha, QStringLiteral(".jpg")),
                 sha.toLower() + QStringLiteral(".jpg"));
        QCOMPARE(fileNameForHash(sha, QString()), sha.toLower());
        QVERIFY(isSha256Hex(sha));
        QVERIFY(!isSha256Hex(QStringLiteral("abc")));
        QVERIFY(!isSha256Hex(sha.left(63) + QStringLiteral("g")));
    }

    void masksTagsByNetworkCounts() {
        QHash<QString, int> counts;
        counts.insert(QStringLiteral("creator:alice"), 100);
        counts.insert(QStringLiteral("spamtag"), 2);
        const QStringList tags{QStringLiteral("creator:alice"),
                               QStringLiteral("spamtag"),
                               QStringLiteral("unknown_tag")};
        TagMask m;
        m.minPosts = 10;
        m.dropUnknown = false;
        QCOMPARE(maskTagsByCounts(tags, counts, {}, m),
                 (QStringList{QStringLiteral("creator:alice"),
                              QStringLiteral("unknown_tag")}));
        // Strict mode has no mercy for tags the PTR cannot count.
        m.dropUnknown = true;
        QCOMPARE(maskTagsByCounts(tags, counts, {}, m),
                 QStringList{QStringLiteral("creator:alice")});
        // 0 disables masking.
        m.minPosts = 0;
        QCOMPARE(maskTagsByCounts(tags, counts, {}, m), normalizeTags(tags));
        // An empty count map (extension unavailable) must keep everything
        // when unknown tags are not dropped.
        m.minPosts = 10;
        m.dropUnknown = false;
        QCOMPARE(maskTagsByCounts(tags, {}, {}, m), normalizeTags(tags));
    }

    void recordBuildsNativePost() {
        Record r;
        r.sha256 = QString(64, u'a');
        r.fileName = QString(64, u'a') + QStringLiteral(".png");
        r.ext = QStringLiteral("png");
        r.width = 800;
        r.height = 600;
        r.addedMs = QDateTime::currentMSecsSinceEpoch();
        r.source = QStringLiteral("https://example.test/post/1");
        r.found = true;
        r.tags = {QStringLiteral("creator:alice"),
                  QStringLiteral("character:bob"),
                  QStringLiteral("species:human"),
                  QStringLiteral("rating:safe"),
                  QStringLiteral("title:hello_world"),
                  QStringLiteral("plain_tag")};
        const Post p = r.toPost(QStringLiteral("/tmp/hydrus"));
        QCOMPARE(p.absFile, QStringLiteral("/tmp/hydrus/") + r.fileName);
        QVERIFY(!p.isRemote);
        QCOMPARE(p.siteKey, QStringLiteral("hydrus"));
        QCOMPARE(p.postUrl, r.source);
        QVERIFY(p.hasMeta);
        // Search sees the namespaced tag AND its bare value.
        const QStringList tokens =
            p.tags.split(QChar::Space, Qt::SkipEmptyParts);
        QVERIFY(tokens.contains(QStringLiteral("creator:alice")));
        QVERIFY(tokens.contains(QStringLiteral("alice")));
        QVERIFY(tokens.contains(QStringLiteral("plain_tag")));
        // Native groups for the sidebar/viewer.
        QCOMPARE(p.customTags.value(QStringLiteral("creator")),
                 QStringList{QStringLiteral("alice")});
        QCOMPARE(p.customTags.value(QStringLiteral("species")),
                 QStringList{QStringLiteral("human")});
        QCOMPARE(p.tagsGeneral, QStringLiteral("plain_tag"));
        QCOMPARE(p.rating, QStringLiteral("general")); // PTR safe -> general
        QCOMPARE(p.title, QStringLiteral("hello_world"));
    }

    void dbRoundTrip() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("hydrus.db"));
        Db db;
        QVERIFY(db.open(path));
        QCOMPARE(db.count(), 0);

        Record first;
        first.sha256 = QString(64, u'1');
        first.fileName = first.sha256 + QStringLiteral(".png");
        first.ext = QStringLiteral("png");
        first.bytes = 1234;
        first.addedMs = 1000;
        first.tags = {QStringLiteral("creator:alice")};
        QVERIFY(db.insert(first));
        QVERIFY(!db.insert(first)); // duplicate hash is a no-op
        QCOMPARE(db.count(), 1);
        QVERIFY(db.contains(first.sha256));
        QVERIFY(!db.contains(QString(64, u'2')));

        Record second;
        second.sha256 = QString(64, u'2');
        second.fileName = second.sha256 + QStringLiteral(".jpg");
        second.ext = QStringLiteral("jpg");
        second.addedMs = 2000;
        QVERIFY(db.insert(second));
        const QVector<Record> rows = db.list();
        QCOMPARE(rows.size(), 2);
        QCOMPARE(rows.first().sha256, second.sha256); // newest first

        QVERIFY(db.updateTags(first.sha256,
                              {QStringLiteral("character:bob"),
                               QStringLiteral("beach")},
                              true, 42, 5000));
        Record got = db.record(first.sha256);
        QCOMPARE(got.tags, (QStringList{QStringLiteral("beach"),
                                        QStringLiteral("character:bob")}));
        QVERIFY(got.found);
        QCOMPARE(got.hydrusFileId, qint64(42));

        // Manual edit wins and marks the row.
        QVERIFY(db.updateManualTags(first.sha256,
                                    {QStringLiteral("spam_removed")}));
        got = db.record(first.sha256);
        QCOMPARE(got.tags, QStringList{QStringLiteral("spam_removed")});
        QVERIFY(got.manualOverride);

        // A later miss keeps the manual tags but clears the found flag.
        QVERIFY(db.markMissed(first.sha256));
        got = db.record(first.sha256);
        QVERIFY(!got.found);
        QCOMPARE(got.tags, QStringList{QStringLiteral("spam_removed")});

        QVERIFY(db.updateFile(first.sha256, QStringLiteral("renamed.png"),
                              QStringLiteral("png"), 99));
        got = db.record(first.sha256);
        QCOMPARE(got.fileName, QStringLiteral("renamed.png"));
        QCOMPARE(got.bytes, qint64(99));

        QVERIFY(db.updateTags(first.sha256, {QStringLiteral("fresh")}, true,
                              7, 9000));
        got = db.record(first.sha256);
        QVERIFY(got.manualOverride); // refresh does not clear the override
        QCOMPARE(got.tags, QStringList{QStringLiteral("fresh")});
        db.close();

        Db again;
        QVERIFY(again.open(path));
        QCOMPARE(again.count(), 2);
        QCOMPARE(again.record(second.sha256).fileName, second.fileName);
    }

    void namespaceBucketsAreLocal() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        Db db;
        QVERIFY(db.open(dir.filePath(QStringLiteral("hydrus.db"))));
        Record r;
        r.sha256 = QStringLiteral("aa");
        r.fileName = QStringLiteral("aa.png");
        r.ext = QStringLiteral("png");
        r.tags = {QStringLiteral("creator:alice"),
                  QStringLiteral("character:bob"),
                  QStringLiteral("series:show"),
                  QStringLiteral("studio:acme"),
                  QStringLiteral("meta:sketch"),
                  QStringLiteral("species:cat"),
                  QStringLiteral("beach")};
        QVERIFY(db.insert(r));
        const QHash<QString, QString> buckets = db.namespaceBuckets();
        QCOMPARE(buckets.value(QStringLiteral("alice")),
                 QStringLiteral("artist"));
        QCOMPARE(buckets.value(QStringLiteral("bob")),
                 QStringLiteral("character"));
        QCOMPARE(buckets.value(QStringLiteral("show")),
                 QStringLiteral("copyright"));
        QCOMPARE(buckets.value(QStringLiteral("acme")),
                 QStringLiteral("copyright"));
        QCOMPARE(buckets.value(QStringLiteral("sketch")),
                 QStringLiteral("meta"));
        // Unknown namespaces and unnamespaced tags do not enter the map.
        QVERIFY(!buckets.contains(QStringLiteral("cat")));
        QVERIFY(!buckets.contains(QStringLiteral("beach")));
        db.close();
    }

    void dbRejectsUnusablePaths() {
        Db db;
        QVERIFY(!db.open(QString()));
        QVERIFY(!db.isOpen());
        QCOMPARE(db.count(), 0);
        QVERIFY(db.list().isEmpty());
        QVERIFY(!db.insert(Record{}));
        QVERIFY(!db.updateTags(QStringLiteral("x"), {}, false, 0, 0));
    }
};

QTEST_GUILESS_MAIN(TestHydrusStore)
#include "test_hydrusstore.moc"
