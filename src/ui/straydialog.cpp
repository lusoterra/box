#include "straydialog.h"

#include <QCheckBox>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

StrayDialog::StrayDialog(bool autoFav, QWidget *parent) : QDialog(parent) {
    setWindowTitle(tr("Download/Add Stray"));
    resize(520, 360);
    auto *lay = new QVBoxLayout(this);
    auto *hint = new QLabel(
        tr("Post or media URLs, one per line. Booru post links are read "
           "through their public API (the site is not added to your booru "
           "list); direct image/video links are fetched as-is."),
        this);
    hint->setWordWrap(true);
    lay->addWidget(hint);
    m_urls = new QPlainTextEdit(this);
    m_urls->setPlaceholderText(QStringLiteral("https://…"));
    lay->addWidget(m_urls, 1);
    m_status = new QLabel(this);
    m_status->setStyleSheet(QStringLiteral("color:#4a5068;"));
    m_status->setWordWrap(true);
    lay->addWidget(m_status);
    m_autoFav =
        new QCheckBox(tr("Auto-favourite downloaded/imported strays"), this);
    m_autoFav->setToolTip(tr("They also land in ♥ Favourites"));
    m_autoFav->setChecked(autoFav);
    connect(m_autoFav, &QCheckBox::toggled, this,
            &StrayDialog::autoFavChanged);
    lay->addWidget(m_autoFav);
    auto *row = new QHBoxLayout();
    auto collect = [this] {
        QStringList urls;
        for (const QString &line :
             m_urls->toPlainText().split(u'\n', Qt::SkipEmptyParts)) {
            const QString u = line.trimmed();
            if (!u.isEmpty())
                urls << u;
        }
        if (urls.isEmpty())
            m_status->setText(tr("Paste at least one link first."));
        return urls;
    };
    auto *dlBtn = new QPushButton(tr("Download"), this);
    dlBtn->setToolTip(
        tr("Fetch the links into _Stray (booru links also get their JSON "
           "sidecar)"));
    connect(dlBtn, &QPushButton::clicked, this, [this, collect] {
        const QStringList urls = collect();
        if (urls.isEmpty())
            return;
        m_status->setText(tr("Queued %1 link(s).").arg(urls.size()));
        emit downloadRequested(urls);
    });
    auto *favBtn = new QPushButton(tr("Favourite only"), this);
    favBtn->setToolTip(
        tr("Add the links to ♥ Favourites without downloading the media"));
    connect(favBtn, &QPushButton::clicked, this, [this, collect] {
        const QStringList urls = collect();
        if (urls.isEmpty())
            return;
        m_status->setText(
            tr("Favouriting %1 link(s), no media download.").arg(urls.size()));
        emit favouriteRequested(urls);
    });
    auto *impBtn = new QPushButton(tr("Import files…"), this);
    impBtn->setToolTip(tr("Copy local files into _Stray"));
    connect(impBtn, &QPushButton::clicked, this, [this] {
        const QStringList paths = QFileDialog::getOpenFileNames(
            this, tr("Import stray files"), QDir::homePath(),
            tr("Images and videos (*.jpg *.jpeg *.png *.gif *.webp *.avif *.mp4 *.webm);;All files (*)"));
        if (paths.isEmpty())
            return;
        m_status->setText(tr("Importing %1 file(s).").arg(paths.size()));
        emit importRequested(paths);
    });
    auto *closeBtn = new QPushButton(tr("Close"), this);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);
    row->addWidget(dlBtn);
    row->addWidget(favBtn);
    row->addWidget(impBtn);
    row->addStretch(1);
    row->addWidget(closeBtn);
    lay->addLayout(row);
}

StrayTagDialog::StrayTagDialog(const QString &fileName, const QString &tags,
                               QWidget *parent)
    : QDialog(parent) {
    setWindowTitle(tr("Edit tags - %1").arg(fileName));
    resize(440, 220);
    auto *lay = new QVBoxLayout(this);
    lay->addWidget(new QLabel(
        tr("Space-separated tags. Buckets are assigned automatically when "
           "you save; reopen to fix anything miscategorized."),
        this));
    m_edit = new QPlainTextEdit(this);
    m_edit->setPlainText(tags);
    m_edit->setTabChangesFocus(true); // Tab leaves the box (Save/Cancel),
                                      // it must never become tag text
    lay->addWidget(m_edit, 1);
    auto *row = new QHBoxLayout();
    auto *saveBtn = new QPushButton(tr("Save"), this);
    connect(saveBtn, &QPushButton::clicked, this, &QDialog::accept);
    auto *cancelBtn = new QPushButton(tr("Cancel"), this);
    connect(cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
    row->addStretch(1);
    row->addWidget(cancelBtn);
    row->addWidget(saveBtn);
    lay->addLayout(row);
}

QString StrayTagDialog::tags() const {
    return m_edit ? m_edit->toPlainText() : QString();
}
