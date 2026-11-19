#include "metadatadialog.h"

#include "desktopfiles.h"
#include "settings.h"

#include <QColor>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileInfo>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QMessageBox>
#include <QPalette>
#include <QProcess>
#include <QPushButton>
#include <QTabWidget>
#include <QTextBrowser>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

namespace {
QString esc(const QString &s) {
    return s.toHtmlEscaped();
}

QString htmlValue(const QString &s) {
    if (s.trimmed().isEmpty())
        return QStringLiteral("<span style=\"color:%1\">&mdash;</span>")
            .arg(QPalette().color(QPalette::Disabled, QPalette::Text).name());
    return esc(s);
}

QString chip(const QString &tag, const QColor &bg, const QColor &fg) {
    return QStringLiteral(
               "<span style=\"background:%1;color:%2;padding:1px 5px;\">"
               "%3</span>")
        .arg(bg.name(), fg.name(), esc(tag));
}

QString valueText(const QJsonValue &v) {
    switch (v.type()) {
    case QJsonValue::Bool:
        return v.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    case QJsonValue::Double: {
        const double d = v.toDouble();
        if (d == double(qint64(d)))
            return QString::number(qint64(d));
        return QString::number(d, 'g', 15);
    }
    case QJsonValue::Null:
        return QStringLiteral("null");
    case QJsonValue::String:
        return v.toString();
    case QJsonValue::Array:
        return QStringLiteral("[%1 items]").arg(v.toArray().size());
    case QJsonValue::Object:
        return QStringLiteral("{%1 keys}").arg(v.toObject().size());
    default:
        return {};
    }
}

void addJsonRows(QTreeWidgetItem *parent, const QJsonValue &value) {
    if (value.isObject()) {
        const QJsonObject o = value.toObject();
        for (auto it = o.begin(); it != o.end(); ++it) {
            auto *item = new QTreeWidgetItem(parent);
            item->setText(0, it.key());
            item->setText(1, valueText(it.value()));
            if (it.value().isObject() || it.value().isArray())
                addJsonRows(item, it.value());
        }
    } else if (value.isArray()) {
        const QJsonArray a = value.toArray();
        for (int i = 0; i < a.size(); ++i) {
            auto *item = new QTreeWidgetItem(parent);
            item->setText(0, QStringLiteral("[%1]").arg(i));
            item->setText(1, valueText(a.at(i)));
            if (a.at(i).isObject() || a.at(i).isArray())
                addJsonRows(item, a.at(i));
        }
    }
}

QString tagSection(const QString &name, const QString &tags,
                   const QColor &bg, const QColor &fg) {
    const QStringList list = tags.split(QChar::Space, Qt::SkipEmptyParts);
    if (list.isEmpty())
        return {};
    QString out = QStringLiteral("<p style=\"margin:10px 0 2px 0\"><b>%1</b>"
                                 "</p><p style=\"margin:0\">")
                      .arg(esc(name));
    for (const QString &tag : list)
        out += chip(tag, bg, fg) + u' ';
    out += QStringLiteral("</p>");
    return out;
}
} // namespace

MetadataDialog::MetadataDialog(const Post &post, const QString &jsonPath,
                               const QJsonObject &fallbackRaw, QWidget *parent)
    : QDialog(parent), m_jsonPath(jsonPath) {
    setWindowTitle(tr("Metadata"));
    resize(820, 680);

    QJsonObject raw = fallbackRaw;
    QString parseError;
    if (!m_jsonPath.isEmpty() && QFileInfo::exists(m_jsonPath)) {
        QFile f(m_jsonPath);
        if (f.open(QIODevice::ReadOnly)) {
            QJsonParseError err;
            const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
            if (err.error == QJsonParseError::NoError && doc.isObject())
                raw = doc.object();
            else if (err.error != QJsonParseError::NoError)
                parseError = err.errorString();
        } else {
            parseError = f.errorString();
        }
    }

    auto *lay = new QVBoxLayout(this);
    auto *tabs = new QTabWidget(this);

    // ── Overview ────────────────────────────────────────────────
    auto *overview = new QTextBrowser(this);
    overview->setOpenExternalLinks(true);
    const QPalette pal = overview->palette();
    const QColor muted = pal.color(QPalette::Disabled, QPalette::Text);
    const QColor chipBg = pal.color(QPalette::Highlight);
    const QColor chipFg = pal.color(QPalette::HighlightedText);
    QString html;
    const QString title = !post.title.trimmed().isEmpty()
                              ? post.title
                              : QStringLiteral("%1 #%2")
                                    .arg(post.siteName.isEmpty()
                                             ? tr("Post")
                                             : post.siteName)
                                    .arg(post.id);
    html += QStringLiteral("<h2 style=\"margin:0 0 10px 0\">%1</h2>")
                .arg(esc(title));
    auto row = [&](const QString &label, const QString &value) {
        if (value.trimmed().isEmpty())
            return;
        html += QStringLiteral(
                    "<tr><td style=\"color:%1;padding:2px 14px 2px 0;"
                    "white-space:nowrap\">%2</td><td>%3</td></tr>")
                    .arg(muted.name(), esc(label), htmlValue(value));
    };
    html += QStringLiteral("<table cellspacing=\"0\" cellpadding=\"0\">");
    row(tr("Site"), post.siteName);
    row(tr("Post ID"), post.id > 0 ? QString::number(post.id) : QString());
    row(tr("Rating"), post.rating);
    row(tr("Score"), post.score != 0 ? QString::number(post.score) : QString());
    row(tr("Dimensions"),
        post.width > 0 && post.height > 0
            ? QStringLiteral("%1 x %2").arg(post.width).arg(post.height)
            : QString());
    row(tr("Type"), post.ext);
    row(tr("Date"), post.date);
    row(tr("Source"), post.source);
    row(tr("File"), post.absFile);
    row(tr("JSON sidecar"), m_jsonPath);
    html += QStringLiteral("</table>");
    if (!post.postUrl.isEmpty()) {
        html += QStringLiteral("<p style=\"margin:8px 0 0 0\">%1: "
                               "<a href=\"%2\">%2</a></p>")
                    .arg(esc(tr("Post page")),
                         esc(post.postUrl));
    }
    if (!post.fileUrl.isEmpty()) {
        html += QStringLiteral("<p style=\"margin:2px 0 0 0\">%1: "
                               "<a href=\"%2\">%2</a></p>")
                    .arg(esc(tr("Media URL")), esc(post.fileUrl));
    }
    html += tagSection(tr("Artist"), post.tagsArtist, chipBg, chipFg);
    html += tagSection(tr("Character"), post.tagsCharacter, chipBg, chipFg);
    html += tagSection(tr("Copyright"), post.tagsCopyright, chipBg, chipFg);
    html += tagSection(tr("Meta"), post.tagsMeta, chipBg, chipFg);
    html += tagSection(tr("General"), post.tagsGeneral, chipBg, chipFg);
    for (auto it = post.customTags.begin(); it != post.customTags.end(); ++it)
        html += tagSection(it.key(), it.value().join(u' '), chipBg, chipFg);
    const bool noBuckets =
        post.tagsArtist.isEmpty() && post.tagsCharacter.isEmpty() &&
        post.tagsCopyright.isEmpty() && post.tagsMeta.isEmpty() &&
        post.tagsGeneral.isEmpty() && post.customTags.isEmpty();
    if (noBuckets && !post.tags.trimmed().isEmpty())
        html += tagSection(tr("Tags"), post.tags, chipBg, chipFg);
    else if (noBuckets && post.tags.trimmed().isEmpty())
        html += QStringLiteral("<p style=\"color:%1\">%2</p>")
                    .arg(muted.name(), esc(tr("No tags recorded.")));
    overview->setHtml(html);
    tabs->addTab(overview, tr("Overview"));

    // ── Raw JSON ────────────────────────────────────────────────
    auto *tree = new QTreeWidget(this);
    tree->setColumnCount(2);
    tree->setHeaderLabels({tr("Key"), tr("Value")});
    tree->setAlternatingRowColors(true);
    tree->header()->setStretchLastSection(true);
    tree->setColumnWidth(0, 240);
    if (!raw.isEmpty()) {
        addJsonRows(tree->invisibleRootItem(), raw);
        tree->expandToDepth(1);
    } else {
        auto *item = new QTreeWidgetItem(tree);
        item->setText(0, parseError.isEmpty()
                             ? tr("No JSON metadata found for this post")
                             : tr("Cannot read the JSON sidecar: %1")
                                   .arg(parseError));
    }
    tabs->addTab(tree, tr("JSON"));
    lay->addWidget(tabs, 1);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    auto *openJsonBtn = buttons->addButton(
        tr("Open JSON in editor…"), QDialogButtonBox::ActionRole);
    auto *showFolderBtn = buttons->addButton(
        tr("Show in folder"), QDialogButtonBox::ActionRole);
    const bool hasJson = !m_jsonPath.isEmpty() && QFileInfo::exists(m_jsonPath);
    openJsonBtn->setEnabled(hasJson);
    showFolderBtn->setEnabled(hasJson);
    connect(openJsonBtn, &QPushButton::clicked, this, &MetadataDialog::openJson);
    connect(showFolderBtn, &QPushButton::clicked, this,
            &MetadataDialog::showFolder);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    lay->addWidget(buttons);
}

void MetadataDialog::openJson() {
    if (m_jsonPath.isEmpty() || !QFileInfo::exists(m_jsonPath))
        return;
    const AppSettings s = loadSettings();
    const QString cmd = s.defaultTextEditor.trimmed();
    if (!cmd.isEmpty()) {
        QStringList parts = QProcess::splitCommand(cmd);
        if (!parts.isEmpty()) {
            QStringList args = parts.mid(1);
            bool placed = false;
            for (QString &a : args) {
                if (a.contains(QStringLiteral("{file}"))) {
                    a.replace(QStringLiteral("{file}"), m_jsonPath);
                    placed = true;
                }
                if (a.contains(QStringLiteral("{url}"))) {
                    a.replace(QStringLiteral("{url}"), m_jsonPath);
                    placed = true;
                }
            }
            if (!placed)
                args << m_jsonPath;
            if (QProcess::startDetached(parts.first(), args))
                return;
        }
    }
    // No configured editor: ask before handing the JSON to the system
    // default (which may not be a text editor at all).
    if (QMessageBox::question(
            this, tr("Open JSON"),
            tr("No default text editor is configured for Box.\n\n"
               "Open %1 with the system default application?")
                .arg(QFileInfo(m_jsonPath).fileName()),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::Yes) == QMessageBox::Yes)
        QDesktopServices::openUrl(QUrl::fromLocalFile(m_jsonPath));
}

void MetadataDialog::showFolder() {
    if (!m_jsonPath.isEmpty())
        DesktopFiles::showInFolder(m_jsonPath);
}
