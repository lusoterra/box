// Read-only sharing catalog: Danbooru JSON, search, tag counts, dedupe.
#include "sharecatalog.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

namespace {
Post makePost(const QString &tag, const QString &artist = {}) {
    Post p;
    p.md5 = QStringLiteral("0123456789abcdef0123456789abcdef");
    p.rating = QStringLiteral("general");
    p.date = QStringLiteral("2026-11-24T12:00:00Z");
    p.tagsGeneral = tag;
    p.tagsArtist = artist;
    p.tags = tag;
    if (!artist.isEmpty())
        p.tags += u' ' + artist;
    p.ext = QStringLiteral("png");
    p.width = 640;
    p.height = 480;
    p.postUrl = QStringLiteral("https://example.test/posts/1");
    p.siteName = QStringLiteral("Local");
    p.source = p.postUrl;
    p.fileUrl = QStringLiteral("https://cdn.test/1.png");
    return p;
}
} // namespace

class TestShareCatalog : public QObject {
    Q_OBJECT
private slots:
    void addsAndSerializesDanbooruShape() {
        ShareCatalog c;
        Post p = makePost(QStringLiteral("alpha beta"),
                          QStringLiteral("alice"));
        p.id = 42;
        p.siteName = QStringLiteral("Danbooru");
        QVERIFY(c.add(p));
        QCOMPARE(c.size(), 1);
        const auto items = c.search(QString(), 1, 40);
        QCOMPARE(items.size(), 1);
        const QJsonObject o = c.postJson(*items.first(), "http://lan:7071");
        QVERIFY(o.value(QStringLiteral("id")).toInteger() > 0);
        QCOMPARE(o.value(QStringLiteral("file_url")).toString(),
                 QStringLiteral("http://lan:7071/share/file/%1")
                     .arg(o.value(QStringLiteral("id")).toInteger()));
        QCOMPARE(o.value(QStringLiteral("preview_file_url")).toString(),
                 QStringLiteral("http://lan:7071/share/thumb/%1")
                     .arg(o.value(QStringLiteral("id")).toInteger()));
        QCOMPARE(o.value(QStringLiteral("tag_string_artist")).toString(),
                 QStringLiteral("alice"));
        QVERIFY(o.value(QStringLiteral("tag_string_general"))
                    .toString()
                    .contains(QStringLiteral("alpha")));
        const QJsonArray arr = c.postsJson(items, "http://lan:7071");
        QCOMPARE(arr.size(), 1);
    }

    void dedupesByHashAndMergesTags() {
        ShareCatalog c;
        Post a = makePost(QStringLiteral("alpha"));
        a.absFile = QStringLiteral("/nonexistent/a.png");
        Post b = makePost(QStringLiteral("beta"));
        // Hash is the shared identity even with different rows/paths.
        b.md5 = a.md5;
        b.tags = QStringLiteral("beta");
        b.tagsGeneral = QStringLiteral("beta");
        QVERIFY(c.add(a));
        QVERIFY(c.add(b));
        QCOMPARE(c.size(), 1);
        QCOMPARE(c.items().first().post.tags,
                 QStringLiteral("alpha beta"));
    }

    void searchHonoursTagsRatingsAndIds() {
        ShareCatalog c;
        Post p = makePost(QStringLiteral("alpha beta"),
                          QStringLiteral("alice"));
        c.add(p);
        Post q = makePost(QStringLiteral("gamma"), QStringLiteral("bob"));
        q.md5 = QStringLiteral("ffffffffffffffffffffffffffffffff");
        q.fileUrl = QStringLiteral("https://cdn.test/2.png");
        q.postUrl = QStringLiteral("https://example.test/posts/2");
        q.rating = QStringLiteral("explicit");
        c.add(q);
        QCOMPARE(c.search(QStringLiteral("alpha"), 1, 40).size(), 1);
        QCOMPARE(c.search(QStringLiteral("alpha -beta"), 1, 40).size(), 0);
        QCOMPARE(c.search(QStringLiteral("rating:general"), 1, 40).size(), 1);
        QCOMPARE(c.search(QStringLiteral("rating:explicit"), 1, 40).size(), 1);
        const auto first = c.search(QString(), 1, 1);
        QCOMPARE(first.size(), 1);
        const qint64 id = first.first()->id;
        QCOMPARE(c.search(QStringLiteral("id:%1").arg(id), 1, 40).size(), 1);
        QCOMPARE(c.search(QStringLiteral("md5:%1").arg(q.md5), 1, 40).size(),
                 1);
        int total = 0;
        c.search(QString(), 1, 40, &total);
        QCOMPARE(total, 2);
    }

    void tagCountsAndCategories() {
        ShareCatalog c;
        c.add(makePost(QStringLiteral("alpha beta"),
                       QStringLiteral("alice")));
        Post q = makePost(QStringLiteral("beta"), QStringLiteral("bob"));
        q.md5 = QStringLiteral("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
        q.fileUrl = QStringLiteral("https://cdn.test/2.png");
        q.postUrl = QStringLiteral("https://example.test/posts/2");
        c.add(q);
        const QJsonArray tags = c.tagsJson(QStringLiteral("be"), 40);
        QCOMPARE(tags.size(), 1);
        const QJsonObject beta = tags.first().toObject();
        QCOMPARE(beta.value(QStringLiteral("name")).toString(),
                 QStringLiteral("beta"));
        QCOMPARE(beta.value(QStringLiteral("post_count")).toInt(), 2);
        const QJsonArray artists = c.tagsJson(QStringLiteral("ali"), 40);
        QCOMPARE(artists.size(), 1);
        QCOMPARE(artists.first().toObject()
                     .value(QStringLiteral("category"))
                     .toInt(),
                 1);
    }

    void remoteOnlyNeedsAUrlAndUsesPreview() {
        ShareCatalog c;
        Post p = makePost(QStringLiteral("alpha"));
        p.isRemote = true;
        p.fileUrl = QStringLiteral("https://cdn.test/1.jpg");
        p.previewUrl = QStringLiteral("https://cdn.test/1.preview.jpg");
        p.absFile.clear();
        p.md5.clear();
        p.postUrl.clear();
        QVERIFY(c.add(p));
        const auto items = c.search(QString(), 1, 10);
        QCOMPARE(items.size(), 1);
        const QJsonObject o = c.postJson(*items.first(), "http://lan:7071");
        QCOMPARE(o.value(QStringLiteral("preview_file_url")).toString(),
                 QStringLiteral("https://cdn.test/1.preview.jpg"));
        Post noMedia;
        noMedia.tags = QStringLiteral("orphan");
        QVERIFY(!c.add(noMedia));
    }

    void stableIds() {
        QCOMPARE(ShareCatalog::stableId(QStringLiteral("a")),
                 ShareCatalog::stableId(QStringLiteral("a")));
        QVERIFY(ShareCatalog::stableId(QStringLiteral("a")) !=
                ShareCatalog::stableId(QStringLiteral("b")));
        QVERIFY(ShareCatalog::stableId(QStringLiteral("a")) <=
                ((1LL << 53) - 1));
        QVERIFY(ShareCatalog::stableId(QStringLiteral("a")) > 0);
    }
};

QTEST_GUILESS_MAIN(TestShareCatalog)
#include "test_sharecatalog.moc"
