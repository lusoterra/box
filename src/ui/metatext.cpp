#include "metatext.h"

#include <QRegularExpression>
#include <QStringList>

namespace {

const QChar kTokenOpen(0x01);
const QChar kTokenClose(0x02);

QString tokenFor(QStringList &stash, const QString &html) {
    stash.append(html);
    return QString(kTokenOpen) + u'M' + QString::number(stash.size() - 1) +
           kTokenClose;
}

QString restoreTokens(QString text, const QStringList &stash) {
    // Highest index first: token 1 must not eat the "1" of token 10.
    for (qsizetype i = stash.size() - 1; i >= 0; --i) {
        text.replace(QString(kTokenOpen) + u'M' + QString::number(i) +
                         kTokenClose,
                     stash.at(i));
    }
    return text;
}

QString trimmedUrl(QString url) {
    static const QString trail = QStringLiteral(".,!?:;)]}");
    while (!url.isEmpty() && trail.contains(url.back()))
        url.chop(1);
    return url;
}

// One clickable anchor. Invalid/dangerous schemes render as plain text so
// a hostile description cannot smuggle javascript:/file: targets into a
// QLabel that opens external links.
QString linkHtml(const QString &label, const QString &href, const QUrl &base) {
    QUrl target(href.trimmed());
    if (target.isRelative() && !base.isEmpty() && base.isValid())
        target = base.resolved(target);
    const QString scheme = target.scheme().toLower();
    if (scheme != QStringLiteral("http") && scheme != QStringLiteral("https") &&
        scheme != QStringLiteral("mailto"))
        return label.toHtmlEscaped();
    return QStringLiteral(
               "<a href=\"%1\" style=\"color:#7c9ef8;\">%2</a>")
        .arg(target.toString(QUrl::FullyEncoded).toHtmlEscaped(),
             label.toHtmlEscaped());
}

void replaceTag(QString &text, const QString &tag, const QString &open,
                const QString &close) {
    // Loop so nested same-tag spans still close in the right order.
    const QRegularExpression re(
        QStringLiteral("\\[%1\\]([\\s\\S]*?)\\[/%1\\]").arg(tag),
        QRegularExpression::CaseInsensitiveOption);
    for (int pass = 0; pass < 8; ++pass) {
        const QString next =
            QString(text).replace(re, open + QStringLiteral("\\1") + close);
        if (next == text)
            break;
        text = next;
    }
}

void stashPattern(QString &text, const QRegularExpression &re,
                  const QString &open, const QString &close,
                  QStringList &stash) {
    QString out;
    qsizetype last = 0;
    auto it = re.globalMatch(text);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        out += text.mid(last, m.capturedStart() - last);
        out += tokenFor(stash, open + m.captured(1).toHtmlEscaped() + close);
        last = m.capturedEnd();
    }
    out += text.mid(last);
    text = out;
}

} // namespace

namespace MetaText {

QString toHtml(const QString &raw, const QUrl &base, int maxChars,
               Flavor flavor) {
    QString text = raw;
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    text.replace(u'\r', u'\n');
    if (maxChars > 0 && text.size() > maxChars)
        text = text.left(maxChars).trimmed() + QStringLiteral("…");

    QStringList stash;

    // Code first: its contents must not be linked or emphasised.
    if (flavor != Flavor::Plain) {
        if (flavor == Flavor::Markdown) {
            static const QRegularExpression fenced(
                QStringLiteral("```[^\\n]*\\n([\\s\\S]*?)```"));
            stashPattern(text, fenced,
                         QStringLiteral("<pre style=\"white-space:pre-wrap;"
                                        "background:#1b2033;color:#d7dcea;"
                                        "padding:6px;border-radius:4px;\">"),
                         QStringLiteral("</pre>"), stash);
        } else {
            static const QRegularExpression bbCode(
                QStringLiteral("\\[code\\]([\\s\\S]*?)\\[/code\\]"),
                QRegularExpression::CaseInsensitiveOption);
            stashPattern(text, bbCode,
                         QStringLiteral("<pre style=\"white-space:pre-wrap;"
                                        "background:#1b2033;color:#d7dcea;"
                                        "padding:6px;border-radius:4px;\">"),
                         QStringLiteral("</pre>"), stash);
        }
        static const QRegularExpression inlineCode(
            QStringLiteral("`([^`\\n]+)`"));
        stashPattern(text, inlineCode,
                     QStringLiteral("<code style=\"background:#20263a;"
                                    "color:#d7dcea;\">"),
                     QStringLiteral("</code>"), stash);
    }

    // Explicit links: BBCode [url=...]x[/url] / [url]x[/url] (DText) and
    // Markdown [label](target) / image embeds (rendered as a text link;
    // QLabel never fetches remote images).
    struct LinkRe {
        const QRegularExpression *re;
        int hrefGroup;
        int labelGroup;
    };
    QList<LinkRe> linkRes;
    static const QRegularExpression bbUrlEq(
        QStringLiteral("\\[url=([^\\]]+)\\]([\\s\\S]*?)\\[/url\\]"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression bbUrl(
        QStringLiteral("\\[url\\]([\\s\\S]*?)\\[/url\\]"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression mdLink(
        QStringLiteral("!?\\[([^\\]]*)\\]\\(([^\\s)]+)\\)"));
    if (flavor == Flavor::DText)
        linkRes = {LinkRe{&bbUrlEq, 1, 2}, LinkRe{&bbUrl, 1, 1}};
    else if (flavor == Flavor::Markdown)
        linkRes = {LinkRe{&mdLink, 2, 1}};
    for (const LinkRe &lr : linkRes) {
        QString out;
        qsizetype last = 0;
        auto it = lr.re->globalMatch(text);
        while (it.hasNext()) {
            const QRegularExpressionMatch m = it.next();
            out += text.mid(last, m.capturedStart() - last);
            out += tokenFor(stash,
                            linkHtml(m.captured(lr.labelGroup),
                                     m.captured(lr.hrefGroup), base));
            last = m.capturedEnd();
        }
        out += text.mid(last);
        text = out;
    }

    // Bare URLs (and www. shorthand) become clickable in every flavour.
    static const QRegularExpression bare(
        QStringLiteral("(?<![\\w/@])((?:https?://|www\\.)[^\\s<>\"'`]+)"),
        QRegularExpression::CaseInsensitiveOption);
    {
        QString out;
        qsizetype last = 0;
        auto it = bare.globalMatch(text);
        while (it.hasNext()) {
            const QRegularExpressionMatch m = it.next();
            const QString shown = trimmedUrl(m.captured(1));
            if (shown.isEmpty())
                continue;
            const qsizetype start = m.capturedStart(1);
            out += text.mid(last, start - last);
            const QString href = shown.startsWith(QStringLiteral("www."),
                                                  Qt::CaseInsensitive)
                                     ? QStringLiteral("https://") + shown
                                     : shown;
            out += tokenFor(stash, linkHtml(shown, href, base));
            last = start + shown.size();
        }
        out += text.mid(last);
        text = out;
    }

    text = text.toHtmlEscaped();

    if (flavor == Flavor::Markdown) {
        // Line-level Markdown (on the escaped text: '>' is already '&gt;').
        static const QRegularExpression heading(
            QStringLiteral("^(#{1,6})\\s+(.*)$"),
            QRegularExpression::MultilineOption);
        text.replace(heading,
                     QStringLiteral("<b style=\"font-size:13px;\">\\2</b>"));
        static const QRegularExpression quoteLine(
            QStringLiteral("^&gt;\\s?(.*)$"),
            QRegularExpression::MultilineOption);
        text.replace(
            quoteLine,
            QStringLiteral("<span style=\"color:#9aa0b8;\">│ \\1</span>"));
        static const QRegularExpression bullet(
            QStringLiteral("^[-*+]\\s+(.*)$"),
            QRegularExpression::MultilineOption);
        text.replace(bullet, QStringLiteral("• \\1"));

        // Emphasis (explicit links are already tokenised).
        static const QRegularExpression strike(
            QStringLiteral("~~(?!\\s)(.+?)(?<!\\s)~~"));
        text.replace(strike, QStringLiteral("<s>\\1</s>"));
        static const QRegularExpression boldStar(
            QStringLiteral("\\*\\*(?!\\s)(.+?)(?<!\\s)\\*\\*"));
        text.replace(boldStar, QStringLiteral("<b>\\1</b>"));
        static const QRegularExpression boldUnderscore(
            QStringLiteral("(?<![\\w])__(?!\\s)(.+?)(?<!\\s)__(?![\\w])"));
        text.replace(boldUnderscore, QStringLiteral("<b>\\1</b>"));
        static const QRegularExpression italicStar(
            QStringLiteral("\\*(?!\\s)(.+?)(?<!\\s)\\*"));
        text.replace(italicStar, QStringLiteral("<i>\\1</i>"));
        static const QRegularExpression italicUnderscore(
            QStringLiteral("(?<![\\w])_(?!\\s)(.+?)(?<!\\s)_(?![\\w])"));
        text.replace(italicUnderscore, QStringLiteral("<i>\\1</i>"));
    } else if (flavor == Flavor::DText) {
        // DText/BBCode spans.
        replaceTag(text, QStringLiteral("b"), QStringLiteral("<b>"),
                   QStringLiteral("</b>"));
        replaceTag(text, QStringLiteral("i"), QStringLiteral("<i>"),
                   QStringLiteral("</i>"));
        replaceTag(text, QStringLiteral("u"), QStringLiteral("<u>"),
                   QStringLiteral("</u>"));
        replaceTag(text, QStringLiteral("s"), QStringLiteral("<s>"),
                   QStringLiteral("</s>"));
        replaceTag(text, QStringLiteral("sup"), QStringLiteral("<sup>"),
                   QStringLiteral("</sup>"));
        replaceTag(text, QStringLiteral("sub"), QStringLiteral("<sub>"),
                   QStringLiteral("</sub>"));
        replaceTag(text, QStringLiteral("quote"),
                   QStringLiteral("<span style=\"color:#9aa0b8;\">"),
                   QStringLiteral("</span>"));
        replaceTag(text, QStringLiteral("spoiler"),
                   QStringLiteral("<span style=\"color:#6f7891;\">"),
                   QStringLiteral("</span>"));
        // [color=#abc]...[/color]: only literal colours, never CSS
        // injections.
        static const QRegularExpression bbColor(
            QStringLiteral("\\[color=(#[0-9a-fA-F]{3,8}|[a-zA-Z]{3,20})\\]"
                           "([\\s\\S]*?)\\[/color\\]"),
            QRegularExpression::CaseInsensitiveOption);
        text.replace(bbColor,
                     QStringLiteral("<span style=\"color:\\1;\">\\2</span>"));
    }

    text.replace(u'\n', QStringLiteral("<br>"));
    return restoreTokens(text, stash);
}

QString toPlain(const QString &raw, Flavor flavor) {
    QString text = raw;
    if (flavor == Flavor::Markdown) {
        static const QRegularExpression fenced(
            QStringLiteral("```[\\s\\S]*?```"));
        text.remove(fenced);
        static const QRegularExpression inlineCode(QStringLiteral("`([^`]*)`"));
        text.replace(inlineCode, QStringLiteral("\\1"));
        static const QRegularExpression mdImage(
            QStringLiteral("!\\[([^\\]]*)\\]\\([^)]*\\)"));
        text.replace(mdImage, QStringLiteral("\\1"));
        static const QRegularExpression mdLink(
            QStringLiteral("\\[([^\\]]*)\\]\\([^)]*\\)"));
        text.replace(mdLink, QStringLiteral("\\1"));
        static const QRegularExpression headingStart(
            QStringLiteral("^#{1,6}\\s+"),
            QRegularExpression::MultilineOption);
        text.remove(headingStart);
        static const QRegularExpression quoteStart(
            QStringLiteral("^>\\s?"), QRegularExpression::MultilineOption);
        text.remove(quoteStart);
        static const QRegularExpression bulletStart(
            QStringLiteral("^[-*+]\\s+"),
            QRegularExpression::MultilineOption);
        text.remove(bulletStart);
        static const QRegularExpression boldStar(
            QStringLiteral("\\*\\*([^*]+?)\\*\\*"));
        text.replace(boldStar, QStringLiteral("\\1"));
        static const QRegularExpression boldUnderscore(
            QStringLiteral("(?<![\\w])__([^_]+?)__(?![\\w])"));
        text.replace(boldUnderscore, QStringLiteral("\\1"));
        static const QRegularExpression strike(
            QStringLiteral("~~([^~]+?)~~"));
        text.replace(strike, QStringLiteral("\\1"));
        static const QRegularExpression italicStar(
            QStringLiteral("\\*([^*\\n]+?)\\*"));
        text.replace(italicStar, QStringLiteral("\\1"));
        static const QRegularExpression italicUnderscore(
            QStringLiteral("(?<![\\w])_([^_\\n]+?)_(?![\\w])"));
        text.replace(italicUnderscore, QStringLiteral("\\1"));
    } else if (flavor == Flavor::DText) {
        static const QRegularExpression bbCode(
            QStringLiteral("\\[code\\][\\s\\S]*?\\[/code\\]"),
            QRegularExpression::CaseInsensitiveOption);
        text.remove(bbCode);
        static const QRegularExpression inlineCode(QStringLiteral("`([^`]*)`"));
        text.replace(inlineCode, QStringLiteral("\\1"));
        static const QRegularExpression bbUrlEq(
            QStringLiteral("\\[url=[^\\]]*\\]([\\s\\S]*?)\\[/url\\]"),
            QRegularExpression::CaseInsensitiveOption);
        text.replace(bbUrlEq, QStringLiteral("\\1"));
        static const QRegularExpression bbUrl(
            QStringLiteral("\\[url\\]([\\s\\S]*?)\\[/url\\]"),
            QRegularExpression::CaseInsensitiveOption);
        text.replace(bbUrl, QStringLiteral("\\1"));
        static const QRegularExpression knownTags(
            QStringLiteral(
                "\\[/?(?:b|i|u|s|sup|sub|quote|spoiler|code|color[^\\]]*|"
                "size[^\\]]*)\\]"),
            QRegularExpression::CaseInsensitiveOption);
        text.remove(knownTags);
    }
    static const QRegularExpression extraBlank(QStringLiteral("\\n{3,}"));
    text.replace(extraBlank, QStringLiteral("\n\n"));
    return text.trimmed();
}

} // namespace MetaText
