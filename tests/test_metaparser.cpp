// Sidecar metadata parsing: LOSN/testfile, Danbooru/Gelbooru native,
// flat gallery-dl, explicit inference, sidecar refresh merges.
#include "metaparser.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QCoreApplication>
#include <QtTest>

namespace {
QJsonObject tagObj(const char *name, const char *type) {
    QJsonObject o;
    o[QStringLiteral("fullString")] = QString::fromLatin1(name);
    o[QStringLiteral("tagType")] = QString::fromLatin1(type);
    return o;
}

QJsonObject losnSidecar(const QStringList &names, const QString &rating = QStringLiteral("g")) {
    QJsonObject o;
    o[QStringLiteral("serverId")] = QStringLiteral("11984812");
    o[QStringLiteral("score")] = QStringLiteral("7");
    o[QStringLiteral("rating")] = rating;
    QJsonArray tags;
    for (const QString &n : names)
        tags.append(tagObj(n.toLatin1().constData(), "none"));
    o[QStringLiteral("tags")] = tags;
    o[QStringLiteral("sources")] =
        QJsonArray{QStringLiteral("https://x.com/u/status/1")};
    o[QStringLiteral("postDate")] = QStringLiteral("2026-08-13T04:54:42.580");
    o[QStringLiteral("postURL")] = QStringLiteral("https://danbooru.donmai.us/posts/11984812");
    o[QStringLiteral("width")] = 100;
    o[QStringLiteral("height")] = 200;
    return o;
}
} // namespace

class TestMetaParser : public QObject {
    Q_OBJECT
private slots:
    void losnBucketsAndUntyped() {
        QJsonObject o = losnSidecar({});
        QJsonArray tags;
        tags.append(tagObj("1girl", "none"));
        tags.append(tagObj("some_artist", "artist"));
        tags.append(tagObj("some_char", "character"));
        tags.append(tagObj("some_series", "copyright"));
        tags.append(tagObj("tall_image", "meta"));
        o[QStringLiteral("tags")] = tags;
        Post p;
        QVERIFY(MetaParser::parse(o, p));
        QCOMPARE(p.id, 11984812LL);
        QCOMPARE(p.score, 7);
        QCOMPARE(p.rating, QStringLiteral("general"));
        QVERIFY(!p.ratingInferred);
        QCOMPARE(p.tagsArtist, QStringLiteral("some_artist"));
        QCOMPARE(p.tagsCharacter, QStringLiteral("some_char"));
        QCOMPARE(p.tagsCopyright, QStringLiteral("some_series"));
        QCOMPARE(p.tagsMeta, QStringLiteral("tall_image"));
        QCOMPARE(p.untyped, QStringLiteral("1girl")); // "none" stays reclassifiable
        QVERIFY(p.tagsGeneral.contains(QStringLiteral("1girl")));
        QCOMPARE(p.source, QStringLiteral("https://x.com/u/status/1"));
        QCOMPARE(p.postUrl, QStringLiteral("https://danbooru.donmai.us/posts/11984812"));
        QCOMPARE(p.width, 100);
        QCOMPARE(p.height, 200);
        QVERIFY(p.hasMeta);
    }

    void losnExplicitInference() {
        Post p;
        QVERIFY(MetaParser::parse(losnSidecar({QStringLiteral("1girl"), QStringLiteral("blowjob")}), p));
        QCOMPARE(p.rating, QStringLiteral("explicit"));
        QVERIFY(p.ratingInferred);
    }

    void losnAmbiguousTagStaysGeneral() {
        // Regression guard for the deliberate "breasts" exclusion.
        Post p;
        QVERIFY(MetaParser::parse(losnSidecar({QStringLiteral("breasts")}), p));
        QCOMPARE(p.rating, QStringLiteral("general"));
        QVERIFY(!p.ratingInferred);
    }

    void losnAlreadyExplicitNotInferred() {
        Post p;
        QVERIFY(MetaParser::parse(losnSidecar({QStringLiteral("blowjob")}, QStringLiteral("e")), p));
        QCOMPARE(p.rating, QStringLiteral("explicit"));
        QVERIFY(!p.ratingInferred);
    }

    void danbooruNative() {
        QJsonObject o;
        o[QStringLiteral("id")] = 42;
        QJsonObject score;
        score[QStringLiteral("total")] = 5;
        o[QStringLiteral("score")] = score;
        o[QStringLiteral("rating")] = QStringLiteral("s");
        o[QStringLiteral("tag_string_artist")] = QStringLiteral("some_artist");
        o[QStringLiteral("tag_string_character")] = QStringLiteral("");
        o[QStringLiteral("tag_string_copyright")] = QStringLiteral("some_series");
        o[QStringLiteral("tag_string_general")] = QStringLiteral("1girl sky");
        o[QStringLiteral("tag_string_meta")] = QStringLiteral("tall_image");
        o[QStringLiteral("image_width")] = 800;
        o[QStringLiteral("image_height")] = 1200;
        o[QStringLiteral("source")] = QStringLiteral("https://example.com");
        o[QStringLiteral("created_at")] = QStringLiteral("2024-01-01T00:00:00Z");
        Post p;
        QVERIFY(MetaParser::parse(o, p));
        QCOMPARE(p.id, 42LL);
        QCOMPARE(p.score, 5);
        QCOMPARE(p.rating, QStringLiteral("sensitive"));
        QCOMPARE(p.tagsArtist, QStringLiteral("some_artist"));
        QCOMPARE(p.title, QStringLiteral("some_artist")); // first artist
        QCOMPARE(p.width, 800);
        QCOMPARE(p.height, 1200);
        QCOMPARE(p.date, QStringLiteral("2024-01-01T00:00:00Z"));
    }

    void danbooruNativeFallbacks() {
        QJsonObject o;
        o[QStringLiteral("id")] = 7;
        o[QStringLiteral("score")] = 3;
        o[QStringLiteral("rating")] = QStringLiteral("g");
        o[QStringLiteral("tag_string")] = QStringLiteral("solo"); // no _general
        o[QStringLiteral("width")] = 640;                         // no image_width
        o[QStringLiteral("height")] = 480;
        o[QStringLiteral("sources")] =
            QJsonArray{QStringLiteral(""), QStringLiteral("https://src.example")};
        Post p;
        QVERIFY(MetaParser::parse(o, p));
        QCOMPARE(p.tagsGeneral, QStringLiteral("solo"));
        QCOMPARE(p.width, 640);
        QCOMPARE(p.height, 480);
        QCOMPARE(p.source, QStringLiteral("https://src.example"));
        QVERIFY(p.title.isEmpty()); // no artist
    }

    void gelbooruNativeBlob() {
        QJsonObject o;
        o[QStringLiteral("id")] = 9;
        o[QStringLiteral("score")] = QStringLiteral("11");
        o[QStringLiteral("rating")] = QStringLiteral("general");
        o[QStringLiteral("tags")] = QStringLiteral("1girl sky school_uniform");
        o[QStringLiteral("width")] = 500;
        o[QStringLiteral("height")] = 500;
        o[QStringLiteral("source")] = QStringLiteral("s");
        o[QStringLiteral("created_at")] = QStringLiteral("d");
        Post p;
        QVERIFY(MetaParser::parse(o, p));
        QCOMPARE(p.tagsGeneral, QStringLiteral("1girl sky school_uniform"));
        QCOMPARE(p.untyped, QStringLiteral("1girl sky school_uniform"));
        QCOMPARE(p.score, 11);
        QVERIFY(p.hasMeta);
    }

    void strayTagsLandInGeneralAndUntyped() {
        // Hand-tagged strays: flat tag string with the stray marker.
        // General for display AND untyped so the classify pipeline
        // (manual overrides + online fetch) can categorize them.
        QJsonObject o;
        o[QStringLiteral("tags")] = QStringLiteral("hatsune_miku sky");
        o[QStringLiteral("file_ext")] = QStringLiteral("jpg");
        o[QStringLiteral("md5")] = QStringLiteral("d41d8cd98f00b204e9800998ecf8427e");
        o[QStringLiteral("stray")] = true;
        Post p;
        QVERIFY(MetaParser::parse(o, p));
        QCOMPARE(p.tagsGeneral, QStringLiteral("hatsune_miku sky"));
        QCOMPARE(p.untyped, QStringLiteral("hatsune_miku sky"));
        QVERIFY(p.tags.contains(QStringLiteral("hatsune_miku")));
        QVERIFY(p.tagsArtist.isEmpty());
        QVERIFY(p.hasMeta);
    }

    void galleryDlFlatMergesCombined() {
        QJsonObject o;
        o[QStringLiteral("id")] = 3;
        o[QStringLiteral("score")] = 1;
        o[QStringLiteral("rating")] = QStringLiteral("g");
        o[QStringLiteral("tags_artist")] = QStringLiteral("a");
        o[QStringLiteral("tags_general")] = QStringLiteral("1girl");
        o[QStringLiteral("tags")] = QStringLiteral("1girl sky"); // dup + new
        Post p;
        QVERIFY(MetaParser::parse(o, p));
        QCOMPARE(p.tagsGeneral, QStringLiteral("1girl sky")); // merged, deduped
        QVERIFY(p.tags.contains(QStringLiteral("sky")));
    }

    void szuruNativeSidecar() {
        QJsonObject o;
        o[QStringLiteral("id")] = 12;
        o[QStringLiteral("safety")] = QStringLiteral("unsafe");
        QJsonArray tags;
        QJsonObject artist;
        artist[QStringLiteral("names")] = QJsonArray{QStringLiteral("an_artist")};
        artist[QStringLiteral("category")] = QStringLiteral("artist");
        tags.append(artist);
        QJsonObject series;
        series[QStringLiteral("names")] = QJsonArray{QStringLiteral("some_series")};
        series[QStringLiteral("category")] = QStringLiteral("series");
        tags.append(series);
        QJsonObject dirty;
        dirty[QStringLiteral("names")] = QJsonArray{QStringLiteral("bad,tag")};
        dirty[QStringLiteral("category")] = QStringLiteral("general");
        tags.append(dirty);
        o[QStringLiteral("tags")] = tags;
        o[QStringLiteral("canvasWidth")] = 1000;
        o[QStringLiteral("canvasHeight")] = 500;
        Post p;
        QVERIFY(MetaParser::parse(o, p));
        QCOMPARE(p.id, 12LL);
        QCOMPARE(p.rating, QStringLiteral("explicit"));
        QCOMPARE(p.tagsArtist, QStringLiteral("an_artist"));
        QCOMPARE(p.tagsCopyright, QStringLiteral("some_series"));
        QVERIFY(!p.tags.contains(QStringLiteral("bad,tag")));
        QVERIFY(p.hasMeta);
    }

    void szuruCustomCategoriesSurvive() {
        // Instance-native categories (meme, ...) are preserved as custom
        // buckets instead of dissolving into general, and stay searchable
        // through the combined tag blob.
        QJsonObject o;
        auto maketag = [](const char *name, const char *cat) {
            QJsonObject t;
            QJsonArray names;
            names.append(QString::fromLatin1(name));
            t[QStringLiteral("names")] = names;
            t[QStringLiteral("category")] = QString::fromLatin1(cat);
            return t;
        };
        QJsonArray tags;
        tags.append(maketag("an_artist", "artist"));
        tags.append(maketag("dank_meme", "meme"));
        tags.append(maketag("oc_guy", "oc"));
        o[QStringLiteral("tags")] = tags;
        o[QStringLiteral("safety")] = QStringLiteral("safe");
        Post p;
        QVERIFY(MetaParser::parse(o, p));
        QCOMPARE(p.tagsArtist, QStringLiteral("an_artist"));
        QVERIFY(!p.tagsGeneral.contains(QStringLiteral("dank_meme")));
        QCOMPARE(p.customTags.value(QStringLiteral("meme")),
                 QStringList({QStringLiteral("dank_meme")}));
        QCOMPARE(p.customTags.value(QStringLiteral("oc")),
                 QStringList({QStringLiteral("oc_guy")}));
        QVERIFY(p.tags.contains(QStringLiteral("dank_meme")));
        QVERIFY(p.tags.contains(QStringLiteral("oc_guy")));
        QVERIFY(p.untyped.isEmpty()); // typed natively: nothing to classify
    }

    void routingByShape() {
        Post p;
        QJsonObject md5marker;
        md5marker[QStringLiteral("md5String")] = QStringLiteral("abc");
        md5marker[QStringLiteral("tags")] = QStringLiteral("solo");
        QVERIFY(MetaParser::parse(md5marker, p)); // LOSN branch
        QCOMPARE(p.tagsGeneral, QStringLiteral("solo"));
    }

    void refreshLosnRetypesTags() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString path = tmp.filePath(QStringLiteral("post.json"));
        QJsonObject cur = losnSidecar({QStringLiteral("1girl")});
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(QJsonDocument(cur).toJson(QJsonDocument::Compact));
        f.close();

        QJsonObject fresh;
        fresh[QStringLiteral("tag_string_artist")] = QStringLiteral("fresh_artist");
        fresh[QStringLiteral("tag_string_general")] = QStringLiteral("1girl sky");
        fresh[QStringLiteral("score")] = 9;
        fresh[QStringLiteral("rating")] = QStringLiteral("s");
        QVERIFY(writeRefreshedSidecar(path, fresh, QStringLiteral("danbooru")));

        QFile f2(path);
        QVERIFY(f2.open(QIODevice::ReadOnly));
        const QJsonObject back =
            QJsonDocument::fromJson(f2.readAll()).object();
        QCOMPARE(back[QStringLiteral("rating")].toString(), QStringLiteral("sensitive"));
        QCOMPARE(back[QStringLiteral("score")].toString(), QStringLiteral("9"));
        QSet<QString> types;
        for (const QJsonValue &v : back[QStringLiteral("tags")].toArray())
            types.insert(v.toObject()[QStringLiteral("fullString")].toString() + u'=' +
                         v.toObject()[QStringLiteral("tagType")].toString());
        QVERIFY(types.contains(QStringLiteral("fresh_artist=artist")));
        QVERIFY(types.contains(QStringLiteral("sky=general")));
    }

    void refreshNativeReplacesWholesale() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString path = tmp.filePath(QStringLiteral("post.json"));
        QJsonObject cur;
        cur[QStringLiteral("id")] = 1;
        cur[QStringLiteral("tag_string_general")] = QStringLiteral("old");
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(QJsonDocument(cur).toJson(QJsonDocument::Compact));
        f.close();

        QJsonObject fresh;
        fresh[QStringLiteral("id")] = 2;
        fresh[QStringLiteral("tag_string_general")] = QStringLiteral("new");
        QVERIFY(writeRefreshedSidecar(path, fresh, QStringLiteral("danbooru")));
        QFile f2(path);
        QVERIFY(f2.open(QIODevice::ReadOnly));
        const QJsonObject back =
            QJsonDocument::fromJson(f2.readAll()).object();
        QCOMPARE(back[QStringLiteral("id")].toDouble(), 2.0);
        QCOMPARE(back[QStringLiteral("tag_string_general")].toString(),
                 QStringLiteral("new"));
    }

    void refreshRejectsUnknown() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString path = tmp.filePath(QStringLiteral("post.json"));
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("{\"foo\":1}");
        f.close();
        QVERIFY(!writeRefreshedSidecar(path, QJsonObject(), QStringLiteral("danbooru")));
        QVERIFY(!writeRefreshedSidecar(tmp.filePath(QStringLiteral("missing.json")),
                                       QJsonObject(), QStringLiteral("danbooru")));
    }
    void refuseArchiveWriteGaps() {
        // src/library/metaparser.cpp:440
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        QDir().mkpath(tmp.filePath(QStringLiteral("dl")));
        const QString dl = tmp.filePath(QStringLiteral("dl"));
        QVERIFY(!refuseArchiveWrite(dl + QStringLiteral("/x.json"), dl));
        QVERIFY(refuseArchiveWrite(tmp.filePath(QStringLiteral("other/x.json")), dl));
        QVERIFY(refuseArchiveWrite(QString(), dl));
        QVERIFY(refuseArchiveWrite(dl + QStringLiteral("/x.json"), QString()));
    }

    void writeRefreshedSidecarGaps() {
        // src/library/metaparser.cpp:449
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString dl = tmp.filePath(QStringLiteral("dl"));
        QDir().mkpath(dl);
        // losn sidecar gets re-typed tag arrays
        const QString losnPath = tmp.filePath(QStringLiteral("losn.json"));
        QJsonObject losn;
        losn[QStringLiteral("serverId")] = 1;
        losn[QStringLiteral("postURL")] = QStringLiteral("https://x/posts/1");
        losn[QStringLiteral("md5String")] = QStringLiteral("abc");
        losn[QStringLiteral("tags")] = QStringLiteral("a b");
        QJsonDocument losnDoc(losn);
        QFile lf(losnPath);
        QVERIFY(lf.open(QIODevice::WriteOnly));
        lf.write(losnDoc.toJson());
        lf.close();
        QJsonObject fresh;
        fresh[QStringLiteral("tag_string_general")] = QStringLiteral("c d");
        QVERIFY(writeRefreshedSidecar(losnPath, fresh, QStringLiteral("danbooru")));
        QFile lf2(losnPath);
        QVERIFY(lf2.open(QIODevice::ReadOnly));
        const QJsonObject updated = QJsonDocument::fromJson(lf2.readAll()).object();
        QVERIFY(updated.contains(QStringLiteral("tags")));
        // native sidecar replaced wholesale
        const QString nativePath = tmp.filePath(QStringLiteral("native.json"));
        QJsonObject native;
        native[QStringLiteral("tag_string")] = QStringLiteral("a b");
        native[QStringLiteral("id")] = 1;
        QJsonDocument nativeDoc(native);
        QFile nf(nativePath);
        QVERIFY(nf.open(QIODevice::WriteOnly));
        nf.write(nativeDoc.toJson());
        nf.close();
        QJsonObject fresh2;
        fresh2[QStringLiteral("id")] = 2;
        QVERIFY(writeRefreshedSidecar(nativePath, fresh2, QStringLiteral("danbooru")));
        QFile nf2(nativePath);
        QVERIFY(nf2.open(QIODevice::ReadOnly));
        const QJsonObject updated2 = QJsonDocument::fromJson(nf2.readAll()).object();
        QCOMPARE(updated2.value(QStringLiteral("id")).toInt(), 2);
        // unknown shape -> false
        const QString unknownPath = tmp.filePath(QStringLiteral("unknown.json"));
        QJsonObject unknown;
        unknown[QStringLiteral("foo")] = QStringLiteral("bar");
        QJsonDocument unknownDoc(unknown);
        QFile uf(unknownPath);
        QVERIFY(uf.open(QIODevice::WriteOnly));
        uf.write(unknownDoc.toJson());
        uf.close();
        QVERIFY(!writeRefreshedSidecar(unknownPath, fresh, QStringLiteral("danbooru")));
        // oversized sidecar -> false
        const QString bigPath = tmp.filePath(QStringLiteral("big.json"));
        QFile bf(bigPath);
        QVERIFY(bf.open(QIODevice::WriteOnly));
        QByteArray big(8 * 1024 * 1024 + 10, 'x');
        bf.write(big);
        bf.close();
        QVERIFY(!writeRefreshedSidecar(bigPath, fresh, QStringLiteral("danbooru")));
    }
};

int main(int argc, char *argv[]) {
    // QCoreApplication (not appless): QSqlDatabase and friends
    // require an application instance. Still no display needed.
    QCoreApplication app(argc, argv);
    TestMetaParser tc;
    return QTest::qExec(&tc, argc, argv);
}
#include "test_metaparser.moc"
