// MetaText: untrusted commentary/comment text -> Qt rich text.
#include "metatext.h"

#include <QCoreApplication>
#include <QUrl>
#include <QtTest>

namespace {
const auto kPlain = MetaText::Flavor::Plain;
const auto kDText = MetaText::Flavor::DText;
const auto kMarkdown = MetaText::Flavor::Markdown;
} // namespace

class TestMetaText : public QObject {
    Q_OBJECT
private slots:
    void bbCode() {
        const QString html = MetaText::toHtml(
            QStringLiteral("[b]bold[/b] [i]ital[/i] [u]under[/u]"), {}, 0,
            kDText);
        QVERIFY(html.contains(QStringLiteral("<b>bold</b>")));
        QVERIFY(html.contains(QStringLiteral("<i>ital</i>")));
        QVERIFY(html.contains(QStringLiteral("<u>under</u>")));
        // Markdown markers stay literal on a DText field.
        const QString lit =
            MetaText::toHtml(QStringLiteral("**not bold**"), {}, 0, kDText);
        QVERIFY(lit.contains(QStringLiteral("**not bold**")));
    }

    void markdown() {
        const QString html = MetaText::toHtml(
            QStringLiteral("**bold** *ital* ~~gone~~ `code`"), {}, 0,
            kMarkdown);
        QVERIFY(html.contains(QStringLiteral("<b>bold</b>")));
        QVERIFY(html.contains(QStringLiteral("<i>ital</i>")));
        QVERIFY(html.contains(QStringLiteral("<s>gone</s>")));
        QVERIFY(html.contains(QStringLiteral("<code")));
        QVERIFY(html.contains(QStringLiteral("code</code>")));
        // BBCode-looking text is literal in a Markdown field.
        const QString lit = MetaText::toHtml(QStringLiteral("[b]x[/b]"), {}, 0,
                                             kMarkdown);
        QVERIFY(lit.contains(QStringLiteral("[b]x[/b]")));
    }

    void explicitLinks() {
        const QString dtext = MetaText::toHtml(
            QStringLiteral("[url=https://e.com/x]text[/url]"), {}, 0, kDText);
        QVERIFY(dtext.contains(QStringLiteral("href=\"https://e.com/x\"")));
        QVERIFY(dtext.contains(QStringLiteral(">text</a>")));
        const QString md = MetaText::toHtml(
            QStringLiteral("[label](https://e.com/y)"), {}, 0, kMarkdown);
        QVERIFY(md.contains(QStringLiteral("href=\"https://e.com/y\"")));
        QVERIFY(md.contains(QStringLiteral(">label</a>")));
    }

    void bareUrls() {
        const QString html = MetaText::toHtml(
            QStringLiteral("see https://e.com/a?b=1, and www.e.com/b."));
        QVERIFY(html.contains(QStringLiteral("href=\"https://e.com/a?b=1\"")));
        QVERIFY(html.contains(QStringLiteral("href=\"https://www.e.com/b\"")));
        QVERIFY(!html.contains(QStringLiteral("a?b=1,")));
    }

    void relativeLinksResolve() {
        const QUrl base(QStringLiteral("https://derpibooru.org"));
        const QString html = MetaText::toHtml(
            QStringLiteral("[tags](/tags/artist:foo)"), base, 0, kMarkdown);
        QVERIFY(html.contains(
            QStringLiteral("href=\"https://derpibooru.org/tags/artist:foo\"")));
        // Without a base the relative target stays plain text.
        const QString plain = MetaText::toHtml(
            QStringLiteral("[tags](/tags/artist:foo)"), {}, 0, kMarkdown);
        QVERIFY(!plain.contains(QStringLiteral("href=")));
        QVERIFY(plain.contains(QStringLiteral("tags")));
    }

    void escapesUntrustedHtml() {
        const QString html = MetaText::toHtml(
            QStringLiteral("<script>alert('x')</script> & <b>raw</b>"), {}, 0,
            kDText);
        QVERIFY(html.contains(QStringLiteral("&lt;script&gt;")));
        QVERIFY(!html.contains(QStringLiteral("<script>")));
        QVERIFY(html.contains(QStringLiteral("&amp;")));
        // Raw HTML-looking tags are escaped; literal BBCode still formats.
        QVERIFY(html.contains(QStringLiteral("&lt;b&gt;raw&lt;/b&gt;")));
    }

    void dangerousSchemesStayPlain() {
        const QString dtext = MetaText::toHtml(
            QStringLiteral("[url=javascript:alert(1)]click[/url]"), {}, 0,
            kDText);
        QVERIFY(!dtext.contains(QStringLiteral("href=")));
        QVERIFY(dtext.contains(QStringLiteral("click")));
        const QString md = MetaText::toHtml(
            QStringLiteral("[x](file:///etc/passwd)"), {}, 0, kMarkdown);
        QVERIFY(!md.contains(QStringLiteral("href=")));
        QVERIFY(md.contains(QStringLiteral("x")));
    }

    void newlinesAndQuotes() {
        const QString html = MetaText::toHtml(
            QStringLiteral("line1\nline2\n> quoted\n- bullet"), {}, 0,
            kMarkdown);
        QVERIFY(html.contains(QStringLiteral("line1<br>line2")));
        QVERIFY(html.contains(QStringLiteral("│ quoted")));
        QVERIFY(html.contains(QStringLiteral("• bullet")));
        // Plain fields keep their Markdown markers (no accidental italics).
        const QString plain = MetaText::toHtml(
            QStringLiteral("(*^^*)"), {}, 0, kPlain);
        QVERIFY(plain.contains(QStringLiteral("(*^^*)")));
    }

    void maxCharsTruncates() {
        const QString html =
            MetaText::toHtml(QStringLiteral("0123456789"), {}, 4);
        QVERIFY(html.startsWith(QStringLiteral("0123")));
        QVERIFY(html.contains(QStringLiteral("…")));
        QVERIFY(!html.contains(QStringLiteral("456789")));
    }

    void plainPreviewStripsMarkup() {
        const QString md = MetaText::toPlain(
            QStringLiteral("**hi** [there](https://e.com) `code`"), kMarkdown);
        QVERIFY(md.contains(QStringLiteral("hi")));
        QVERIFY(md.contains(QStringLiteral("there")));
        QVERIFY(md.contains(QStringLiteral("code")));
        QVERIFY(!md.contains(QStringLiteral("**")));
        QVERIFY(!md.contains(QStringLiteral("https://")));
        const QString dt = MetaText::toPlain(
            QStringLiteral("[b]bold[/b] [url=https://e.com]there[/url]"),
            kDText);
        QVERIFY(dt.contains(QStringLiteral("bold")));
        QVERIFY(dt.contains(QStringLiteral("there")));
        QVERIFY(!dt.contains(QStringLiteral("[b]")));
        QVERIFY(!dt.contains(QStringLiteral("https://")));
        // Plain notes are shown verbatim.
        QCOMPARE(MetaText::toPlain(QStringLiteral("(*^^*)"), kPlain),
                 QStringLiteral("(*^^*)"));
    }
};

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    TestMetaText tc;
    return QTest::qExec(&tc, argc, argv);
}
#include "test_metatext.moc"
