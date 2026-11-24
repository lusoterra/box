#include "hydrustagdialog.h"

#include "hydrusstore.h"

#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

HydrusTagDialog::HydrusTagDialog(const QString &fileName, const QString &prompt,
                                 const QStringList &tags, QWidget *parent)
    : QDialog(parent) {
    setWindowTitle(tr("Hydrus tags"));
    setModal(true);
    resize(520, 460);
    auto *lay = new QVBoxLayout(this);
    auto *title = new QLabel(fileName, this);
    title->setWordWrap(true);
    QFont f = title->font();
    f.setBold(true);
    title->setFont(f);
    lay->addWidget(title);
    if (!prompt.isEmpty()) {
        auto *p = new QLabel(prompt, this);
        p->setWordWrap(true);
        lay->addWidget(p);
    }
    auto *hint = new QLabel(
        tr("One tag per line. Namespaced tags keep their Hydrus namespace "
           "(creator:alice, character:bob, series:…, species:…). Plain "
           "lines are General."),
        this);
    hint->setWordWrap(true);
    hint->setStyleSheet(QStringLiteral("color:#4a5068;"));
    lay->addWidget(hint);
    m_edit = new QPlainTextEdit(this);
    m_edit->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_edit->setPlaceholderText(QStringLiteral("creator:alice\ncharacter:bob\n"
                                             "plain_tag"));
    m_edit->setPlainText(HydrusStore::normalizeTags(tags).join(u'\n'));
    lay->addWidget(m_edit, 1);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save |
                                             QDialogButtonBox::Cancel,
                                         this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    lay->addWidget(buttons);
}

QStringList HydrusTagDialog::tags() const {
    if (!m_edit)
        return {};
    QStringList raw;
    for (const QString &line : m_edit->toPlainText().split(u'\n')) {
        const QString t = line.trimmed();
        if (!t.isEmpty())
            raw << t;
    }
    return HydrusStore::normalizeTags(raw);
}

HydrusRefreshPreviewDialog::HydrusRefreshPreviewDialog(
    const QString &fileName, bool manualOverride, const QStringList &before,
    const QStringList &after, QWidget *parent)
    : QDialog(parent) {
    setWindowTitle(tr("Refresh Hydrus tags"));
    setModal(true);
    resize(640, 480);
    auto *lay = new QVBoxLayout(this);
    auto *title = new QLabel(fileName, this);
    title->setWordWrap(true);
    QFont f = title->font();
    f.setBold(true);
    title->setFont(f);
    lay->addWidget(title);
    auto *note = new QLabel(
        manualOverride
            ? tr("This post was edited by hand. Keep the current tags or "
                 "replace them with the freshly fetched Hydrus tags.")
            : tr("Current tags versus the freshly fetched Hydrus tags "
                 "(after tag masking)."),
        this);
    note->setWordWrap(true);
    lay->addWidget(note);
    auto *cols = new QHBoxLayout();
    const auto makeColumn = [this](const QString &heading,
                                   const QStringList &tags) {
        auto *box = new QVBoxLayout();
        auto *head = new QLabel(heading, this);
        head->setStyleSheet(QStringLiteral("color:#4a5068;font-weight:bold;"));
        box->addWidget(head);
        auto *view = new QPlainTextEdit(this);
        view->setReadOnly(true);
        view->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        view->setPlainText(HydrusStore::normalizeTags(tags).join(u'\n'));
        box->addWidget(view, 1);
        return box;
    };
    cols->addLayout(makeColumn(tr("Current tags"), before), 1);
    cols->addLayout(makeColumn(tr("Refreshed from Hydrus"), after), 1);
    lay->addLayout(cols, 1);
    auto *buttons = new QDialogButtonBox(this);
    QPushButton *keep = buttons->addButton(tr("Keep current"),
                                           QDialogButtonBox::RejectRole);
    QPushButton *replace = buttons->addButton(
        tr("Use refreshed tags"), QDialogButtonBox::AcceptRole);
    QPushButton *cancel = buttons->addButton(tr("Cancel"),
                                             QDialogButtonBox::DestructiveRole);
    // Destructive change: keep the current tags unless the user explicitly
    // picks the refreshed set. Cancel leaves everything untouched.
    keep->setDefault(true);
    connect(keep, &QPushButton::clicked, this, [this] {
        m_choice = Choice::Keep;
        reject();
    });
    connect(replace, &QPushButton::clicked, this, [this] {
        m_choice = Choice::Replace;
        accept();
    });
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    lay->addWidget(buttons);
}
