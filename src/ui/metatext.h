#pragma once

// Rich-text formatting for post text fields that carry site markup:
// Danbooru DText/BBCode, Philomena Markdown, and plain text with bare
// URLs. Output is Qt rich text (QLabel/QTextBrowser). Input is untrusted,
// so the whole document is HTML-escaped first and link targets are
// restricted to http(s)/mailto; site-relative markdown links resolve
// against the booru base URL.
#include <QString>
#include <QUrl>

namespace MetaText {
// Markup family behind a text field. Danbooru comments/notes use DText
// (BBCode-like); Philomena descriptions use Markdown; HyBooru Hydrus notes
// are plain text. All flavours still auto-link bare http(s)/www URLs.
enum class Flavor { Plain, DText, Markdown };
// Raw -> Qt rich text. `base` resolves relative link targets.
// maxChars > 0 truncates the source before formatting.
QString toHtml(const QString &raw, const QUrl &base = {}, int maxChars = 0,
               Flavor flavor = Flavor::DText);
// Markup stripped, suitable for compact previews (no HTML escaping).
QString toPlain(const QString &raw, Flavor flavor = Flavor::DText);
} // namespace MetaText
