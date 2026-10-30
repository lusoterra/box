// HiddenBooru engine: search/detail/tag shapes from the v0.8 source
// tarball (no live calls - Tor-only host). Post pages at /post/<id>/view,
// files served relative (resolved against base).
#include "booruengine.h"
#include "tagsuggest.h"

#include <QCoreApplication>
#include <QUrlQuery>
#include <QtTest>

namespace {
QByteArray searchFixture() {
    return QByteArray(
        R"([{"id":4242,"hash":"1957ed38471bee3f564a1acd11be7ae6","htype":"md5","mtype":"i","fmt":"jpg","size":99111,"width":800,"height":600,"rating":"s","tags":"1girl sky","file_url":"/posts/a1/b2/1957ed38471bee3f564a1acd11be7ae6.jpg","preview_url":"/posts/a1/b2/1957ed38471bee3f564a1acd11be7ae6.thumb.jpg","source_url":"","created":"2026-01-01"},)"
        R"({"id":4243,"hash":"zz9top","htype":"sha1","mtype":"v","fmt":"webm","size":99112,"width":640,"height":480,"rating":"e","tags":"animated","file_url":"https://cdn.example/x.webm","preview_url":"","source_url":"https://example.com/src","created":"2026-01-02"}])");
}
const QString kBase = QStringLiteral("http://hbooru5cef5tkb7a7m5de5kjxqg6abfh3447sxw36n5pe7utnnei25yd.onion");
} // namespace

class TestHiddenbooru : public QObject {
    Q_OBJECT
private slots:
    void searchParsing() {
        const BooruEngineOps *ops = engineOps(BooruEngine::Hiddenbooru);
        const SearchResult r = ops->parseSearch(
            QStringLiteral("hidden"), {BooruEngine::Hiddenbooru, kBase, {}},
            searchFixture());
        QVERIFY(r.ok());
        QCOMPARE(r.posts.size(), 2);
        const Post still = r.posts.at(0);
        QCOMPARE(still.id, 4242LL);
        QCOMPARE(still.ext, QStringLiteral("jpg")); // from fmt
        QCOMPARE(still.fileUrl,
                 kBase + QStringLiteral("/posts/a1/b2/1957ed38471bee3f564a1acd11be7ae6.jpg"));
        QCOMPARE(still.previewUrl,
                 kBase + QStringLiteral("/posts/a1/b2/1957ed38471bee3f564a1acd11be7ae6.thumb.jpg"));
        QCOMPARE(still.postUrl, kBase + QStringLiteral("/post/4242/view"));
        QCOMPARE(still.width, 800);
        QCOMPARE(still.rating, QStringLiteral("general")); // s = safe
        QCOMPARE(still.siteKey, QStringLiteral("hidden"));
        QVERIFY(still.isRemote && still.hasMeta);
        QVERIFY(still.tagsGeneral.split(u' ').contains(QStringLiteral("1girl")));
        QCOMPARE(still.rawJson.value(QStringLiteral("md5")).toString(),
                 QStringLiteral("1957ed38471bee3f564a1acd11be7ae6"));
        const Post clip = r.posts.at(1);
        QCOMPARE(clip.ext, QStringLiteral("webm"));
        QCOMPARE(clip.rating, QStringLiteral("explicit"));
        QCOMPARE(clip.source, QStringLiteral("https://example.com/src"));
        // No md5 key when the hash is not md5.
        QVERIFY(clip.rawJson.value(QStringLiteral("md5")).toString().isEmpty());
    }

    void urls() {
        const BooruEngineOps *ops = engineOps(BooruEngine::Hiddenbooru);
        const BooruEngineOps::Context ctx{BooruEngine::Hiddenbooru, kBase, {}};
        const QUrlQuery q(QUrl(ops->searchUrl(
            ctx, {QStringLiteral("sky")}, 3, 40)).query());
        QCOMPARE(q.queryItemValue(QStringLiteral("terms")), QStringLiteral("sky"));
        QCOMPARE(q.queryItemValue(QStringLiteral("page")), QStringLiteral("3"));
        QCOMPARE(q.queryItemValue(QStringLiteral("limit")), QStringLiteral("40"));
        QCOMPARE(q.queryItemValue(QStringLiteral("order")), QStringLiteral("DESC"));
        // Single-post refresh narrows the search to the id.
        const QUrlQuery rq(QUrl(ops->refreshUrl(ctx, 4242)).query());
        QCOMPARE(rq.queryItemValue(QStringLiteral("terms")),
                 QStringLiteral("id:4242"));
        QCOMPARE(RemotePosts::engineKey(BooruEngine::Hiddenbooru),
                 QStringLiteral("hiddenbooru"));
        QCOMPARE(RemotePosts::engineForKey(QStringLiteral("hiddenbooru")),
                 BooruEngine::Hiddenbooru);
        QCOMPARE(RemotePosts::engineLabel(BooruEngine::Hiddenbooru),
                 QStringLiteral("HiddenBooru"));
    }

    void translateKeepsFiletypeOnDanbooruOnly() {
        QCOMPARE(RemotePosts::translateSearchTags(
                     {QStringLiteral("sky"),
                      QStringLiteral("filetype:gif,webp")},
                     BooruEngine::Danbooru),
                 QStringList({QStringLiteral("sky"),
                              QStringLiteral("filetype:gif,webp")}));
        QCOMPARE(RemotePosts::translateSearchTags(
                     {QStringLiteral("sky"),
                      QStringLiteral("filetype:gif,webp")},
                     BooruEngine::Hiddenbooru),
                 QStringList({QStringLiteral("sky")}));
    }

    void tagsParsing() {
        // Flat array of tag name strings.
        const auto out = TagSuggester::parse(
            BooruEngine::Hiddenbooru,
            QByteArray(R"(["1girl","sky",""] )"));
        QCOMPARE(out.size(), 2);
        QCOMPARE(out.at(0).tag, QStringLiteral("1girl"));
        QCOMPARE(out.at(0).bucket, QStringLiteral("general"));
    }
};

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    TestHiddenbooru tc;
    return QTest::qExec(&tc, argc, argv);
}
#include "test_hiddenbooru.moc"
