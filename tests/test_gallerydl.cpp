// System gallery-dl backend: URL classification, CLI building, version
// parsing (pure, no network/binary) + Pixiv/Pawchive array-tags sidecars.
#include "gallerydl.h"
#include "metaparser.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

class TestGalleryDl : public QObject {
    Q_OBJECT
private slots:
    void galleryHosts() {
        QVERIFY(GalleryDl::handlesUrl(
            QStringLiteral("https://www.pixiv.net/en/artworks/123456")));
        QVERIFY(GalleryDl::handlesUrl(
            QStringLiteral("https://user.fanbox.cc/posts/789")));
        QVERIFY(GalleryDl::handlesUrl(
            QStringLiteral("https://pawchive.pw/patreon/user/1/post/2")));
        QVERIFY(GalleryDl::handlesUrl(
            QStringLiteral("https://pawchive.st/fanbox/user/1")));
        QVERIFY(GalleryDl::handlesUrl(
            QStringLiteral("https://kemono.cr/patreon/user/1/post/2")));
        QVERIFY(
            GalleryDl::handlesUrl(QStringLiteral("https://www.patreon.com/posts/x-1")));
        QVERIFY(GalleryDl::handlesUrl(
            QStringLiteral("https://user.fantia.jp/posts/3")));
    }

    void directMediaStaysNative() {
        QVERIFY(!GalleryDl::handlesUrl(
            QStringLiteral("https://i.pximg.net/img/1.jpg")));
        QVERIFY(!GalleryDl::handlesUrl(
            QStringLiteral("https://example.com/vid.webm")));
        QVERIFY(!GalleryDl::handlesUrl(
            QStringLiteral("https://pawchive.pw/data/ab/file.png?x=1")));
    }

    void booruAndGarbage() {
        // Booru post pages keep the native resolver (bucketed sidecars).
        QVERIFY(!GalleryDl::handlesUrl(
            QStringLiteral("https://danbooru.donmai.us/posts/1")));
        QVERIFY(!GalleryDl::handlesUrl(
            QStringLiteral("https://e621.net/posts/2")));
        QVERIFY(!GalleryDl::handlesUrl(QStringLiteral("not a url")));
        QVERIFY(!GalleryDl::handlesUrl(QStringLiteral("ftp://x/y")));
        QVERIFY(!GalleryDl::handlesUrl(QString()));
    }

    void args() {
        // Transport only: no --write-metadata (box writes its own
        // user-owned sidecars afterwards).
        const QStringList a = GalleryDl::buildArgs(
            QStringLiteral("/dl/_Stray"),
            {QStringLiteral("https://www.pixiv.net/en/artworks/1")});
        QVERIFY(!a.contains(QStringLiteral("--write-metadata")));
        const int d = a.indexOf(QStringLiteral("-d"));
        QVERIFY(d >= 0);
        QCOMPARE(a.at(d + 1), QStringLiteral("/dl/_Stray"));
        QCOMPARE(a.last(),
                 QStringLiteral("https://www.pixiv.net/en/artworks/1"));
    }

    void versionParse() {
        QCOMPARE(GalleryDl::parseVersion(QStringLiteral("1.32.20\n")),
                 QStringLiteral("1.32.20"));
        QCOMPARE(GalleryDl::parseVersion(QStringLiteral("gallery-dl 1.32.20")),
                 QStringLiteral("1.32.20"));
        QVERIFY(GalleryDl::parseVersion(QStringLiteral("nope")).isEmpty());
    }

    void pixivArrayTags() {
        // Pixiv/Pawchive shape: tags as string array, no buckets.
        QJsonObject o;
        o[QStringLiteral("id")] = 123456;
        QJsonArray tags;
        tags.append(QStringLiteral("blue_archive"));
        tags.append(QStringLiteral("artist_name"));
        o[QStringLiteral("tags")] = tags;
        o[QStringLiteral("title")] = QStringLiteral("t");
        Post p;
        QVERIFY(MetaParser::parse(o, p));
        QVERIFY(p.tagsGeneral.split(u' ').contains(QStringLiteral("blue_archive")));
        // Untyped so the classifier buckets them later.
        QVERIFY(p.untyped.split(u' ').contains(QStringLiteral("blue_archive")));
        QVERIFY(p.untyped.split(u' ').contains(QStringLiteral("artist_name")));
    }

    void stringTagsUnchanged() {
        // String form (no id/rating, so the flat parser runs) keeps the
        // old behavior: general only, untyped untouched.
        QJsonObject o;
        o[QStringLiteral("tags_general")] = QStringLiteral("sky");
        o[QStringLiteral("tags")] = QStringLiteral("cloud");
        Post p;
        QVERIFY(MetaParser::parse(o, p));
        QVERIFY(p.tagsGeneral.split(u' ').contains(QStringLiteral("cloud")));
        QVERIFY(p.untyped.isEmpty());
    }

    void sidecarShape() {
        // Own stray sidecar: importer shape + booru keys, tags empty
        // for the user. Never overwrites an existing sidecar.
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString media = dir.path() + QStringLiteral("/pixiv_1.jpg");
        QFile f(media);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("fakebytes");
        f.close();
        const QString url = QStringLiteral("https://www.pixiv.net/en/artworks/1");
        QVERIFY(GalleryDl::writeStraySidecar(media, url));
        QCOMPARE(GalleryDl::sidecarPathFor(media),
                 dir.path() + QStringLiteral("/pixiv_1.json"));
        QFile jf(GalleryDl::sidecarPathFor(media));
        QVERIFY(jf.open(QIODevice::ReadOnly));
        const QJsonObject o =
            QJsonDocument::fromJson(jf.readAll()).object();
        QCOMPARE(o.value(QStringLiteral("tags")).toString(), QString());
        QCOMPARE(o.value(QStringLiteral("file_ext")).toString(),
                 QStringLiteral("jpg"));
        QVERIFY(!o.value(QStringLiteral("md5")).toString().isEmpty());
        QCOMPARE(o.value(QStringLiteral("post_url")).toString(), url);
        QCOMPARE(o.value(QStringLiteral("source")).toString(), url);
        QVERIFY(o.value(QStringLiteral("stray")).toBool());
        // Second call: sidecar exists now, must not clobber.
        QVERIFY(!GalleryDl::writeStraySidecar(media, url));
        // Full parse: first-class stray with the page recorded.
        Post p;
        QVERIFY(MetaParser::parse(o, p));
        QCOMPARE(p.postUrl, url);
        QCOMPARE(p.source, url);
    }

    void adoptFlattensAndKeepsMetadata() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString strayRoot = dir.path();
        QSet<QString> before;
        QFile keep(strayRoot + QStringLiteral("/keep.png"));
        QVERIFY(keep.open(QIODevice::WriteOnly));
        keep.write("keep");
        keep.close();
        before.insert(keep.fileName());

        // gallery-dl layout: <site>/<user>/<file> + its metadata JSON.
        const QString nested = strayRoot + QStringLiteral("/pixiv/123");
        QVERIFY(QDir().mkpath(nested));
        const QString media = nested + QStringLiteral("/123_p0.jpg");
        QFile f(media);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("pixivbytes");
        f.close();
        QJsonObject meta;
        meta[QStringLiteral("url")] =
            QStringLiteral("https://www.pixiv.net/artworks/123");
        meta[QStringLiteral("title")] = QStringLiteral("My work");
        meta[QStringLiteral("artist")] = QStringLiteral("painter");
        meta[QStringLiteral("urls")] =
            QJsonObject{{QStringLiteral("original"),
                         QStringLiteral("https://i.pximg.net/img.jpg")}};
        QFile mf(nested + QStringLiteral("/123_p0.json"));
        QVERIFY(mf.open(QIODevice::WriteOnly));
        mf.write(QJsonDocument(meta).toJson());
        mf.close();

        const QString page = QStringLiteral("https://www.pixiv.net/artworks/123");
        const GalleryDl::AdoptSummary s =
            GalleryDl::adoptNewFiles(strayRoot, before, page);
        QCOMPARE(s.files, 1);
        QCOMPARE(s.sidecars, 1);
        // Flattened with the normal stray naming; nested dirs pruned.
        QVERIFY(!QFile::exists(media));
        QVERIFY(!QDir(nested).exists());
        const QStringList flat = QDir(strayRoot).entryList(QDir::Files);
        QString stray;
        int strayMedia = 0;
        int straySidecars = 0;
        for (const QString &name : flat) {
            if (!name.startsWith(QStringLiteral("_Stray_")))
                continue;
            if (name.endsWith(QStringLiteral(".json")))
                ++straySidecars;
            else {
                ++strayMedia;
                stray = name;
            }
        }
        QCOMPARE(strayMedia, 1);
        QCOMPARE(straySidecars, 1);
        QVERIFY(QFile::exists(strayRoot + QStringLiteral("/keep.png")));
        QVERIFY(stray.endsWith(QStringLiteral(".jpg")));
        const QString sidecar = strayRoot + u'/' + stray.left(stray.lastIndexOf(u'.')) +
                                QStringLiteral(".json");
        QVERIFY(QFile::exists(sidecar));
        QFile sf(sidecar);
        QVERIFY(sf.open(QIODevice::ReadOnly));
        const QJsonObject o = QJsonDocument::fromJson(sf.readAll()).object();
        QCOMPARE(o.value(QStringLiteral("tags")).toString(), QString());
        QCOMPARE(o.value(QStringLiteral("post_url")).toString(), page);
        QCOMPARE(o.value(QStringLiteral("file_url")).toString(),
                 QStringLiteral("https://i.pximg.net/img.jpg"));
        QCOMPARE(o.value(QStringLiteral("origin_dir")).toString(),
                 QStringLiteral("pixiv/123"));
        QCOMPARE(o.value(QStringLiteral("title")).toString(),
                 QStringLiteral("My work"));
        QVERIFY(o.value(QStringLiteral("stray")).toBool());
        // gallery-dl's JSON next to the media is gone.
        QVERIFY(!QFile::exists(nested + QStringLiteral("/123_p0.json")));
    }
};

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    TestGalleryDl tc;
    return QTest::qExec(&tc, argc, argv);
}
#include "test_gallerydl.moc"
