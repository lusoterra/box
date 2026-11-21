// System gallery-dl backend: URL classification, CLI building, version
// parsing (pure, no network/binary) + Pixiv/Pawchive array-tags sidecars.
#include "gallerydl.h"
#include "metaparser.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
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
        const QStringList a = GalleryDl::buildArgs(
            QStringLiteral("/dl/_Stray"),
            {QStringLiteral("https://www.pixiv.net/en/artworks/1")});
        QVERIFY(a.contains(QStringLiteral("--write-metadata")));
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
};

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    TestGalleryDl tc;
    return QTest::qExec(&tc, argc, argv);
}
#include "test_gallerydl.moc"
