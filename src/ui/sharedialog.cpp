#include "sharedialog.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

ShareDialog::ShareDialog(const ShareOptions &current, bool hydrusAvailable,
                         int hydrusCount, bool running, int postCount,
                         const QStringList &urls, QWidget *parent)
    : QDialog(parent), m_current(current) {
    setWindowTitle(tr("Share library on LAN"));
    setModal(true);
    resize(580, 520);

    auto *lay = new QVBoxLayout(this);
    auto *intro = new QLabel(
        tr("Expose the selected posts as a read-only Danbooru API. "
           "LoliSnatcher or another Box instance can add it as a normal "
           "Danbooru entry."),
        this);
    intro->setWordWrap(true);
    lay->addWidget(intro);

    auto *sources = new QGroupBox(tr("What to share"), this);
    auto *sourceLay = new QVBoxLayout(sources);
    m_hydrus = new QCheckBox(
        hydrusAvailable
            ? tr("Hydrus tab (%1 posts)").arg(hydrusCount)
            : tr("Hydrus tab (not enabled / empty)"),
        this);
    m_hydrus->setChecked(hydrusAvailable && current.hydrus);
    m_hydrus->setEnabled(hydrusAvailable);
    m_hydrus->setToolTip(
        hydrusAvailable
            ? tr("Share the isolated Hydrus library and its native tags.")
            : tr("Enable Hydrus integration in Preferences → Hydrus and "
                 "import something first."));
    sourceLay->addWidget(m_hydrus);

    m_downloads = new QCheckBox(tr("Online mode: downloads"), this);
    m_downloads->setChecked(current.downloads);
    m_downloads->setToolTip(
        tr("Every media file under the download folder, with its sidecar "
           "tags."));
    sourceLay->addWidget(m_downloads);

    m_favourites = new QCheckBox(tr("Online mode: favourites"), this);
    m_favourites->setChecked(current.favourites);
    m_favourites->setToolTip(
        tr("Favourite rows. Local files are served directly; remote-only "
           "rows use the media cache or are proxied by Box."));
    sourceLay->addWidget(m_favourites);

    auto *localRow = new QHBoxLayout();
    m_local = new QCheckBox(tr("Local archive folder:"), this);
    m_local->setChecked(current.localFolder && !current.localFolderPath.isEmpty());
    localRow->addWidget(m_local);
    m_folder = new QLineEdit(current.localFolderPath, this);
    m_folder->setPlaceholderText(tr("Choose a read-only archive folder"));
    m_browse = new QPushButton(tr("Browse…"), this);
    localRow->addWidget(m_folder, 1);
    localRow->addWidget(m_browse);
    sourceLay->addLayout(localRow);
    auto *mergeNote = new QLabel(
        tr("Selected sources are merged into one API. Duplicate rows are "
           "collapsed by site+id, content hash, media URL or path."),
        this);
    mergeNote->setWordWrap(true);
    sourceLay->addWidget(mergeNote);
    lay->addWidget(sources);


    auto *net = new QGroupBox(tr("Network"), this);
    auto *netForm = new QFormLayout(net);
    m_bind = new QLineEdit(
        current.bindAddress.isEmpty() ? QStringLiteral("0.0.0.0")
                                      : current.bindAddress,
        this);
    m_bind->setToolTip(
        tr("0.0.0.0 listens on every interface (LAN). 127.0.0.1 or "
           "localhost keeps it local only."));
    netForm->addRow(tr("Bind address:"), m_bind);
    m_port = new QSpinBox(this);
    m_port->setRange(1, 65535);
    m_port->setValue(current.port > 0 ? current.port : 7071);
    netForm->addRow(tr("Port:"), m_port);
    lay->addWidget(net);

    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    if (running) {
        m_status->setStyleSheet(QStringLiteral("color:#2e8b6e;font-weight:bold;"));
        m_status->setText(tr("API is live: %1 post(s).").arg(postCount));
    } else {
        m_status->setStyleSheet(QStringLiteral("color:#4a5068;"));
        m_status->setText(tr("Not sharing."));
    }
    lay->addWidget(m_status);
    auto *urlRow = new QHBoxLayout();
    m_url = new QLineEdit(this);
    m_url->setReadOnly(true);
    m_url->setPlaceholderText(tr("API URL appears here"));
    if (!urls.isEmpty()) {
        m_url->setText(urls.first());
        m_url->setToolTip(urls.join(u'\n'));
    }
    m_copyUrl = new QPushButton(tr("Copy API URL"), this);
    m_copyUrl->setEnabled(!urls.isEmpty());
    connect(m_copyUrl, &QPushButton::clicked, this, [this] {
        if (m_url->text().isEmpty())
            return;
        QApplication::clipboard()->setText(m_url->text());
    });
    urlRow->addWidget(m_url, 1);
    urlRow->addWidget(m_copyUrl);
    lay->addLayout(urlRow);

    auto *warn = new QLabel(
        tr("Read-only. Anyone who can reach this address can browse the "
           "selected posts; there is no authentication."),
        this);
    warn->setWordWrap(true);
    warn->setStyleSheet(QStringLiteral("color:#c0392b;"));
    lay->addWidget(warn);

    auto *buttons = new QHBoxLayout();
    m_stop = new QPushButton(tr("Stop sharing"), this);
    m_stop->setVisible(running);
    m_refresh = new QPushButton(tr("Refresh snapshot"), this);
    m_refresh->setVisible(running);
    m_start = new QPushButton(
        running ? tr("Restart with these sources") : tr("Start sharing"),
        this);
    m_start->setDefault(true);
    auto *close = new QPushButton(tr("Close"), this);
    buttons->addWidget(m_stop);
    buttons->addWidget(m_refresh);
    buttons->addStretch(1);
    buttons->addWidget(m_start);
    buttons->addWidget(close);
    lay->addLayout(buttons);

    connect(close, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_start, &QPushButton::clicked, this, [this] {
        const ShareOptions o = options();
        if (!o.hydrus && !o.downloads && !o.favourites && !o.localFolder) {
            QMessageBox::information(this, tr("Share library"),
                                     tr("Select at least one source."));
            return;
        }
        if (o.localFolder && !QDir(o.localFolderPath).exists()) {
            QMessageBox::information(
                this, tr("Share library"),
                tr("The local archive folder does not exist."));
            return;
        }
        m_action = Action::Start;
        accept();
    });
    connect(m_refresh, &QPushButton::clicked, this, [this] {
        m_action = Action::Refresh;
        accept();
    });
    connect(m_stop, &QPushButton::clicked, this, [this] {
        m_action = Action::Stop;
        accept();
    });
    connect(m_browse, &QPushButton::clicked, this, [this] {
        const QString dir = QFileDialog::getExistingDirectory(
            this, tr("Local archive folder"),
            m_folder->text().isEmpty() ? QDir::homePath() : m_folder->text());
        if (!dir.isEmpty()) {
            m_folder->setText(dir);
            m_local->setChecked(true);
            updateStartState();
        }
    });
    connect(m_local, &QCheckBox::toggled, this, [this](bool on) {
        m_folder->setEnabled(on);
        m_browse->setEnabled(on);
        updateStartState();
    });
    connect(m_folder, &QLineEdit::textChanged, this,
            [this] { updateStartState(); });
    connect(m_hydrus, &QCheckBox::toggled, this,
            [this] { updateStartState(); });
    connect(m_downloads, &QCheckBox::toggled, this,
            [this] { updateStartState(); });
    connect(m_favourites, &QCheckBox::toggled, this,
            [this] { updateStartState(); });
    m_folder->setEnabled(m_local->isChecked());
    m_browse->setEnabled(m_local->isChecked());
    updateStartState();
}

void ShareDialog::updateStartState() {
    const bool any = (m_hydrus && m_hydrus->isChecked()) ||
                     (m_downloads && m_downloads->isChecked()) ||
                     (m_favourites && m_favourites->isChecked()) ||
                     (m_local && m_local->isChecked());
    bool folderOk = true;
    if (m_local && m_local->isChecked())
        folderOk = !m_folder->text().trimmed().isEmpty() &&
                   QDir(m_folder->text().trimmed()).exists();
    if (m_start)
        m_start->setEnabled(any && folderOk);
    if (m_refresh)
        m_refresh->setEnabled(any && folderOk);
}

ShareOptions ShareDialog::options() const {
    ShareOptions o;
    o.hydrus = m_hydrus && m_hydrus->isChecked();
    o.downloads = m_downloads && m_downloads->isChecked();
    o.favourites = m_favourites && m_favourites->isChecked();
    o.localFolder = m_local && m_local->isChecked();
    o.localFolderPath = m_folder ? m_folder->text().trimmed() : QString();
    o.bindAddress = m_bind ? m_bind->text().trimmed() : QStringLiteral("0.0.0.0");
    if (o.bindAddress.isEmpty())
        o.bindAddress = QStringLiteral("0.0.0.0");
    o.port = m_port ? m_port->value() : 7071;
    return o;
}
