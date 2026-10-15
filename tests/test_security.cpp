// Adversarial inputs: a rogue booru (or backup file) must never crash
// the app, escape the download folder, or trigger unbounded
// allocations. Each case feeds hostile data through a real code path
// and asserts contained behavior.
#include "booruengine.h"
#include "downloadmanager.h"
#include "filehash.h"
#include "losnimport.h"
#include "metaparser.h"
#include "queryor.h"
#include "remotesource.h"
#include "sitequirks.h"
#include "webdlbackend.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>

namespace {
bool fileNameSafe(const QString &name) {
    return !name.contains(u'/') && !name.contains(u'\\') &&
           !name.contains(QStringLiteral(".."));
}

// 10 000-deep JSON arrays: parsers must fail cleanly, never recurse
// into a stack overflow.
QByteArray deepJsonArray(int depth) {
    QByteArray body(depth, '[');
    body += QByteArray(depth, ']');
    return body;
}
} // namespace

class TestSecurity : public QObject {
    Q_OBJECT
private slots:
    void fileNameTraversal() {
        // md5 / ext / label straight from a hostile API object.
        {
            QJsonObject raw;
            raw[QStringLiteral("id")] = 7;
            raw[QStringLiteral("md5")] = QStringLiteral("../../etc/passwd");
            raw[QStringLiteral("file_ext")] = QStringLiteral("jpg");
            const QString name =
                RemotePosts::downloadFileName(QStringLiteral("Danbooru"), raw);
            QVERIFY(fileNameSafe(name));
            QVERIFY(name.startsWith(QStringLiteral("Danbooru_7.")));
        }
        {
            QJsonObject raw;
            raw[QStringLiteral("id")] = 7;
            raw[QStringLiteral("md5")] =
                QStringLiteral("d41d8cd98f00b204e9800998ecf8427e");
            raw[QStringLiteral("file_ext")] =
                QStringLiteral("jpg/../../x");
            const QString name =
                RemotePosts::downloadFileName(QStringLiteral("Danbooru"), raw);
            QVERIFY(fileNameSafe(name));
            QVERIFY(name.endsWith(QStringLiteral(".jpg")));
        }
        {
            QJsonObject raw;
            raw[QStringLiteral("file_ext")] = QString(200, u'x');
            const QString name = RemotePosts::downloadFileName(
                QStringLiteral("..."), raw);
            QVERIFY(fileNameSafe(name));
            QVERIFY(name.startsWith(QStringLiteral("Booru_")));
        }
        {
            QJsonObject raw;
            raw[QStringLiteral("file_url")] = QStringLiteral("javascript:alert(1)");
            raw[QStringLiteral("id")] = 9;
            const QString name = RemotePosts::downloadFileName(
                QStringLiteral("Danbooru"), raw);
            QVERIFY(fileNameSafe(name));
            QVERIFY(name.endsWith(QStringLiteral(".jpg")));
        }
        // Uppercase hex still names canonically (lowercased).
        {
            QJsonObject raw;
            raw[QStringLiteral("md5")] =
                QStringLiteral("D41D8CD98F00B204E9800998ECF8427E");
            const QString name = RemotePosts::downloadFileName(
                QStringLiteral("Danbooru"), raw);
            QCOMPARE(name, QStringLiteral(
                               "Danbooru_d41d8cd98f00b204e9800998ecf8427e.jpg"));
        }
    }

    void extOfTraversal() {
        QCOMPARE(BooruEngineOps::extOf(QStringLiteral("http://h/a.b/c"),
                                       QStringLiteral("jpg")),
                 QStringLiteral("jpg"));
        QCOMPARE(BooruEngineOps::extOf(QStringLiteral("http://h/a.jpg"),
                                       QStringLiteral("bin")),
                 QStringLiteral("jpg"));
        QVERIFY(BooruEngineOps::extOf(QStringLiteral("http://h/a.JPG"),
                                      QStringLiteral("bin"))
                    .indexOf(u'/') < 0);
    }

    void schemeGate() {
        using RemotePosts::isHttpUrl;
        QVERIFY(isHttpUrl(QStringLiteral("http://x/y")));
        QVERIFY(isHttpUrl(QStringLiteral("https://x/y")));
        QVERIFY(!isHttpUrl(QStringLiteral("file:///etc/passwd")));
        QVERIFY(!isHttpUrl(QStringLiteral("javascript:alert(1)")));
        QVERIFY(!isHttpUrl(QStringLiteral("data:text/html,hi")));
        QVERIFY(!isHttpUrl(QStringLiteral("gopher://x/1")));
        QVERIFY(!isHttpUrl(QString()));
        QVERIFY(!isHttpUrl(QStringLiteral("notaurl")));
    }

    void hostileMappers() {
        // Wrong types everywhere, hostile numbers, missing fields.
        {
            QJsonObject o;
            o[QStringLiteral("id")] = -5;
            o[QStringLiteral("score")] = 1e30;
            o[QStringLiteral("rating")] = 12345;
            o[QStringLiteral("tag_string_artist")] = QJsonObject{
                {QStringLiteral("nested"), QStringLiteral("nope")}};
            o[QStringLiteral("tag_string_general")] =
                QString(100000, u'x');
            o[QStringLiteral("image_width")] = QStringLiteral("99999999999999999999");
            o[QStringLiteral("file_url")] = QStringLiteral("javascript:alert(1)");
            const Post p = RemotePosts::fromDanbooru(
                o, QStringLiteral("https://danbooru.donmai.us"));
            QCOMPARE(p.id, -5);
            QVERIFY(p.score >= 0); // clamped, never UB-wrapped
            QVERIFY(p.width >= 0);
        }
        {
            QJsonObject o;
            o[QStringLiteral("id")] = QJsonValue::Null;
            o[QStringLiteral("tags")] = 42;
            o[QStringLiteral("width")] = true;
            o[QStringLiteral("file_url")] = QStringLiteral("file:///etc/passwd");
            const Post p = RemotePosts::fromGelbooru(
                o, QStringLiteral("https://gelbooru.com"));
            QCOMPARE(p.id, 0);
            QVERIFY(p.tagsGeneral.isEmpty());
        }
        {
            QJsonObject o;
            o[QStringLiteral("id")] = 3;
            o[QStringLiteral("created_at")] =
                QJsonObject{{QStringLiteral("evil"), 1}};
            o[QStringLiteral("tags")] = QStringLiteral("a");
            const Post p = RemotePosts::fromMoebooru(
                o, QStringLiteral("https://yande.re"));
            QCOMPARE(p.id, 3); // date garbage must not move the id
        }
        {
            QJsonObject o;
            o[QStringLiteral("id")] = 4;
            o[QStringLiteral("tags")] = QStringLiteral("not-an-array");
            o[QStringLiteral("canvasWidth")] = -99;
            const Post p = RemotePosts::fromSzuru(
                o, QStringLiteral("https://szuru.example"));
            QCOMPARE(p.id, 4);
            QVERIFY(p.width >= 0);
        }
        {
            // Empty object: all defaults, no crash.
            const Post p = RemotePosts::fromDanbooru(
                QJsonObject(), QStringLiteral("https://x"));
            QCOMPARE(p.id, 0);
            QVERIFY(p.fileUrl.isEmpty());
        }
    }

    void hostileSidecars() {
        // Hostile VALUES (not malformed JSON: that never reaches us).
        {
            QJsonObject o;
            o[QStringLiteral("id")] = QStringLiteral("not-a-number");
            o[QStringLiteral("tag_string")] = QJsonObject{
                {QStringLiteral("deep"), QJsonArray{1, 2, 3}}};
            o[QStringLiteral("rating")] = QJsonArray{};
            o[QStringLiteral("file_url")] = true;
            o[QStringLiteral("md5")] = QJsonArray{1};
            Post p;
            MetaParser::parse(o, p); // must not crash; result irrelevant
            QVERIFY(p.tags.size() < 1000000);
        }
        {
            // 1MB tag blob: bounded work, no crash.
            QJsonObject o;
            o[QStringLiteral("id")] = 1;
            o[QStringLiteral("tag_string")] =
                QString(1024 * 1024, u'a');
            o[QStringLiteral("rating")] = QStringLiteral("g");
            Post p;
            QElapsedTimer t;
            t.start();
            MetaParser::parse(o, p);
            QVERIFY(t.elapsed() < 5000);
        }
    }

    void deepJsonFailsCleanly() {
        // 3000 nested arrays: QJsonDocument caps depth with an error.
        const QByteArray body = deepJsonArray(3000);
        BooruSite site;
        site.key = QStringLiteral("danbooru");
        site.engine = BooruEngine::Danbooru;
        const BooruEngineOps *ops = engineOps(BooruEngine::Danbooru);
        const BooruEngineOps::Context ctx{BooruEngine::Danbooru,
                                          QStringLiteral("https://x"), {}};
        QElapsedTimer t;
        t.start();
        const SearchResult r = ops->parseSearch(QStringLiteral("danbooru"),
                                                ctx, body);
        QVERIFY(t.elapsed() < 5000);
        QVERIFY(!r.ok()); // error, not a crash
    }

    void shimmieXmlBomb() {
        // Mild entity-expansion bomb + 5000-deep nesting: the streaming
        // reader must finish (error or posts), never hang or blow up.
        QByteArray bomb =
            "<!DOCTYPE posts ["
            "<!ENTITY a \"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\">"
            "<!ENTITY b \"&a;&a;&a;&a;&a;&a;&a;&a;&a;&a;\">"
            "<!ENTITY c \"&b;&b;&b;&b;&b;&b;&b;&b;&b;&b;\">"
            "]><posts>";
        for (int i = 0; i < 10; ++i)
            bomb += "<post id=\"1\" tags=\"&c;\"/>";
        bomb += "</posts>";
        BooruSite site;
        site.key = QStringLiteral("shimmie");
        site.baseUrl = QStringLiteral("https://x");
        QElapsedTimer t;
        t.start();
        const SearchResult r = RemotePosts::parseShimmieXml(site, bomb);
        QVERIFY(t.elapsed() < 10000);
        QVERIFY(r.posts.size() <= 10);
        QByteArray deep = "<posts>";
        for (int i = 0; i < 5000; ++i)
            deep += "<a>";
        for (int i = 0; i < 5000; ++i)
            deep += "</a>";
        deep += "</posts>";
        const SearchResult r2 = RemotePosts::parseShimmieXml(site, deep);
        QVERIFY(r2.posts.isEmpty());
    }

    void queryorPathological() {
        // Unbalanced/group-heavy inputs must terminate fast.
        QElapsedTimer t;
        t.start();
        const auto p1 = QueryOr::parse(QString(5000, u'~') +
                                       QString(5000, u'(') +
                                       QStringLiteral("abc"));
        QVERIFY(t.elapsed() < 2000);
        QVERIFY(p1.andTokens.size() + p1.groups.size() < 10000);
        const QString mega = QString(1024 * 1024, u' ');
        const auto p2 = QueryOr::parse(mega + QStringLiteral("tag"));
        QVERIFY(t.elapsed() < 2000);
        QCOMPARE(p2.andTokens, QStringList{QStringLiteral("tag")});
        // Over-cap expansion refuses instead of fanning out.
        QueryOr::ParsedQuery pq;
        pq.andTokens = QStringList{QStringLiteral("a")};
        for (int i = 0; i < 5; ++i)
            pq.groups << QStringList{QStringLiteral("x"),
                                     QStringLiteral("y"),
                                     QStringLiteral("z")};
        bool over = false;
        QVERIFY(QueryOr::expand(pq, QueryOr::kMaxCombos, over).isEmpty());
        QVERIFY(over);
    }

    void idFromUrlAdversarial() {
        QElapsedTimer t;
        t.start();
        // 100KB of near-miss patterns: linear scan, fast verdict.
        const QString evil = QStringLiteral("/posts/show/").repeated(9000);
        const qint64 id = LosnImport::idFromPostUrl(evil);
        QVERIFY(t.elapsed() < 2000);
        QCOMPARE(id, 0);
        QCOMPARE(LosnImport::idFromPostUrl(
                     QStringLiteral("https://x/posts/99999999999999999999")),
                 0); // overflow: invalid, never UB-wrapped
        QCOMPARE(LosnImport::idFromPostUrl(QString()), 0);
    }

    void cookieNormalizeAdversarial() {
        using RemotePosts::normalizeCookies;
        // 5000 tokens: hard-capped, fast.
        QStringList many;
        for (int i = 0; i < 5000; ++i)
            many << QStringLiteral("a=1");
        QElapsedTimer t;
        t.start();
        const QStringList out = normalizeCookies(many);
        QVERIFY(t.elapsed() < 2000);
        QVERIFY(out.size() <= 50);
        // 1MB value: dropped by the size cap.
        QCOMPARE(normalizeCookies(
                     {QStringLiteral("big=") + QString(1024 * 1024, u'x')})
                     .size(),
                 0);
        // Attribute names never survive as cookies.
        QVERIFY(normalizeCookies({QStringLiteral("Path=/; Secure")}).isEmpty());
    }

    void hostileBackupDb() {
        // A malicious store.db: hostile types, huge fields, negatives.
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("evil.db"));
        const QString conn = QStringLiteral("evil_%1").arg(
            QUuid::createUuid().toString(QUuid::WithoutBraces));
        QSqlDatabase db =
            QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
        db.setDatabaseName(path);
        QVERIFY(db.open());
        {
            QSqlQuery q(db);
            QVERIFY(q.exec(QStringLiteral(
                "CREATE TABLE BooruItem (id INTEGER PRIMARY KEY, "
                "thumbnailURL TEXT, sampleURL TEXT, fileURL TEXT, "
                "postURL TEXT, mediaType TEXT, isSnatched INTEGER, "
                "isFavourite INTEGER)")));
        }
        {
            QSqlQuery q(db);
            QVERIFY(q.exec(QStringLiteral("CREATE TABLE Tag (id INTEGER PRIMARY "
                                          "KEY, name TEXT)")));
        }
        {
            QSqlQuery q(db);
            QVERIFY(q.exec(QStringLiteral("CREATE TABLE ImageTag (tagID INTEGER, "
                                          "booruItemID INTEGER)")));
        }
        {
            QSqlQuery q(db);
            q.prepare(QStringLiteral(
                "INSERT INTO BooruItem VALUES (?,?,?,?,?,?,?,?)"));
            q.addBindValue(-1);
            q.addBindValue(QString(500000, u'x')); // 500KB thumb URL
            q.addBindValue(QByteArray("\x00\x01\x02")); // blob sample URL
            q.addBindValue(QStringLiteral("javascript:alert(1)"));
            q.addBindValue(QString());
            q.addBindValue(QStringLiteral("image"));
            q.addBindValue(9999999999LL);
            q.addBindValue(2);
            QVERIFY(q.exec());
        }
        {
            QSqlQuery q(db);
            q.prepare(QStringLiteral("INSERT INTO Tag VALUES (?,?)"));
            q.addBindValue(1);
            q.addBindValue(QString(500000, u'y')); // 500KB tag name
            QVERIFY(q.exec());
            QSqlQuery q2(db);
            q2.prepare(
                QStringLiteral("INSERT INTO ImageTag VALUES (?,?)"));
            q2.addBindValue(1);
            q2.addBindValue(-1);
            QVERIFY(q2.exec());
        }
        db.close();
        QSqlDatabase::removeDatabase(conn);
        QString error;
        QElapsedTimer t;
        t.start();
        const QList<LosnRow> rows = LosnImport::readBackup(path, error);
        QVERIFY(t.elapsed() < 5000);
        QCOMPARE(rows.size(), 1);
        QCOMPARE(rows.first().rowId, -1); // carried, never trusted for paths
        QVERIFY(rows.first().md5.isEmpty()); // javascript: yields no md5
        // Missing tables: clean error, no rows, no crash.
        const QString path2 = dir.filePath(QStringLiteral("empty.db"));
        {
            QSqlDatabase db2 = QSqlDatabase::addDatabase(
                QStringLiteral("QSQLITE"), conn + QStringLiteral("2"));
            db2.setDatabaseName(path2);
            QVERIFY(db2.open());
            db2.close();
            QSqlDatabase::removeDatabase(conn + QStringLiteral("2"));
        }
        QString error2;
        QVERIFY(LosnImport::readBackup(path2, error2).isEmpty());
        QVERIFY(!error2.isEmpty());
    }

    void planSkipsNonHttp() {
        // file:///javascript:/data: file URLs must never become requests.
        Post evil;
        evil.isRemote = true;
        evil.siteName = QStringLiteral("Danbooru");
        evil.fileUrl = QStringLiteral("file:///etc/passwd");
        evil.rawJson = QJsonObject{{QStringLiteral("id"), 1}};
        QVERIFY(DownloadManager::plan({evil}, QStringLiteral("/d")).isEmpty());
        evil.fileUrl = QStringLiteral("javascript:alert(1)");
        QVERIFY(DownloadManager::plan({evil}, QStringLiteral("/d")).isEmpty());
        Post ok;
        ok.isRemote = true;
        ok.siteName = QStringLiteral("Danbooru");
        ok.fileUrl = QStringLiteral("https://x/y.jpg");
        ok.rawJson = QJsonObject{{QStringLiteral("id"), 2}};
        QCOMPARE(DownloadManager::plan({ok}, QStringLiteral("/d")).size(), 1);
    }

    void htmlSniff() {
        QVERIFY(!fileLooksLikeHtml(QStringLiteral("/nonexistent")));
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString html = dir.filePath(QStringLiteral("w.html"));
        QFile f(html);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("<!DOCTYPE html><html><body>wall</body></html>");
        f.close();
        QVERIFY(fileLooksLikeHtml(html));
        const QString jpg = dir.filePath(QStringLiteral("a.jpg"));
        QFile g(jpg);
        QVERIFY(g.open(QIODevice::WriteOnly));
        g.write(QByteArray::fromHex("ffd8ffe000104a464946"));
        g.close();
        QVERIFY(!fileLooksLikeHtml(jpg));
    }
};

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    TestSecurity tc;
    return QTest::qExec(&tc, argc, argv);
}
#include "test_security.moc"
