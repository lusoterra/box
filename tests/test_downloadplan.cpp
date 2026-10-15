// Batch download planning: per-site layouts, skip rules, filename fallbacks.
#include "downloadmanager.h"

#include <QCoreApplication>
#include <QtTest>

namespace {
Post remotePost(const QString &site, const QString &fileUrl, const QJsonObject &raw) {
    Post p;
    p.isRemote = true;
    p.siteName = site;
    p.id = raw[QStringLiteral("id")].toInt(1);
    p.fileUrl = fileUrl;
    p.rawJson = raw;
    return p;
}
// Favourite re-listed from the DB: no API object, but id + file URL survive.
Post favPost(const QString &site, qint64 id, const QString &fileUrl) {
    Post p;
    p.isRemote = true;
    p.siteName = site;
    p.id = id;
    p.fileUrl = fileUrl;
    p.hasMeta = true;
    return p;
}
} // namespace

class TestDownloadPlan : public QObject {
    Q_OBJECT
private slots:
    void layoutPerSite() {
        QJsonObject raw;
        raw[QStringLiteral("id")] = 7103104;
        raw[QStringLiteral("md5")] = QStringLiteral("516e1f01a05ea895b33316bd28bfd27f");
        raw[QStringLiteral("file_ext")] = QStringLiteral("jpg");
        const QList<DownloadJob> jobs = DownloadManager::plan(
            {remotePost(QStringLiteral("Gelbooru"),
                        QStringLiteral("https://img/x.jpg"), raw)},
            QStringLiteral("/dest"));
        QCOMPARE(jobs.size(), 1);
        const DownloadJob &j = jobs.first();
        QCOMPARE(j.fileName,
                 QStringLiteral("Gelbooru_516e1f01a05ea895b33316bd28bfd27f.jpg"));
        QCOMPARE(j.filePath,
                 QStringLiteral("/dest/Gelbooru/"
                                "Gelbooru_516e1f01a05ea895b33316bd28bfd27f.jpg"));
        QCOMPARE(j.jsonPath,
                 QStringLiteral("/dest/Gelbooru/"
                                "Gelbooru_516e1f01a05ea895b33316bd28bfd27f.json"));
    }

    void skipsUndownloadable() {
        QJsonObject raw;
        raw[QStringLiteral("id")] = 1;
        raw[QStringLiteral("md5")] = QStringLiteral("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
        raw[QStringLiteral("file_ext")] = QStringLiteral("jpg");
        Post walled = remotePost(QStringLiteral("Gelbooru"), QString(), raw); // login-walled
        Post local; // local files are never "downloaded"
        local.absFile = QStringLiteral("/pics/a.jpg");
        QVERIFY(DownloadManager::plan({walled, local}, QStringLiteral("/dest")).isEmpty());
    }

    void idFallbackName() {
        QJsonObject raw;
        raw[QStringLiteral("id")] = 77; // no md5 anywhere
        // Naming prefers the API object; the post itself fills the gaps.
        const QList<DownloadJob> jobs = DownloadManager::plan(
            {remotePost(QStringLiteral("Danbooru"), QStringLiteral("https://x/f.png"),
                        raw)},
            QStringLiteral("/d"));
        QCOMPARE(jobs.size(), 1);
        QCOMPARE(jobs.first().fileName, QStringLiteral("Danbooru_77.png"));
    }

    void favMd5FromFileUrl() {
        // LOSN-import workflow: favourite has no API object, but the file
        // URL carries the md5 stem (and the extension).
        const QList<DownloadJob> jobs = DownloadManager::plan(
            {favPost(QStringLiteral("Gelbooru"), 7103104,
                     QStringLiteral("https://img4.gelbooru.com/images/51/6e/"
                                    "516e1f01a05ea895b33316bd28bfd27f.png"))},
            QStringLiteral("/dest"));
        QCOMPARE(jobs.size(), 1);
        QCOMPARE(jobs.first().fileName,
                 QStringLiteral(
                     "Gelbooru_516e1f01a05ea895b33316bd28bfd27f.png"));
    }

    void favIdFallback() {
        // No md5 stem in the URL: post id still beats Site_0.
        const QList<DownloadJob> jobs = DownloadManager::plan(
            {favPost(QStringLiteral("Danbooru"), 77,
                     QStringLiteral("https://x/f.png"))},
            QStringLiteral("/d"));
        QCOMPARE(jobs.size(), 1);
        QCOMPARE(jobs.first().fileName, QStringLiteral("Danbooru_77.png"));
    }

    void rawObjectWins() {
        // A real API object is never overridden by URL-derived guesses.
        QJsonObject raw;
        raw[QStringLiteral("id")] = 5;
        raw[QStringLiteral("md5")] =
            QStringLiteral("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
        raw[QStringLiteral("file_ext")] = QStringLiteral("jpg");
        const QList<DownloadJob> jobs = DownloadManager::plan(
            {remotePost(
                QStringLiteral("Gelbooru"),
                QStringLiteral("https://img/x/"
                               "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb.png"),
                raw)},
            QStringLiteral("/d"));
        QCOMPARE(jobs.size(), 1);
        QCOMPARE(jobs.first().fileName,
                 QStringLiteral(
                     "Gelbooru_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.jpg"));
    }

    void batchCollisionsDisambiguated() {
        // Two id-less rows with plain URLs: previously both planned as
        // Site_0.ext and the second download overwrote the first.
        const Post a =
            favPost(QStringLiteral("Gelbooru"), 0, QStringLiteral("https://x/a.jpg"));
        const Post b =
            favPost(QStringLiteral("Gelbooru"), 0, QStringLiteral("https://x/b.jpg"));
        const QList<DownloadJob> jobs =
            DownloadManager::plan({a, b}, QStringLiteral("/d"));
        QCOMPARE(jobs.size(), 2);
        QVERIFY(jobs.at(0).filePath != jobs.at(1).filePath);
        QCOMPARE(jobs.at(0).fileName, QStringLiteral("Gelbooru_0.jpg"));
        QCOMPARE(jobs.at(1).fileName, QStringLiteral("Gelbooru_0_2.jpg"));
    }
};

int main(int argc, char *argv[]) {
    // QCoreApplication (not appless): QSqlDatabase and friends
    // require an application instance. Still no display needed.
    QCoreApplication app(argc, argv);
    TestDownloadPlan tc;
    return QTest::qExec(&tc, argc, argv);
}
#include "test_downloadplan.moc"
