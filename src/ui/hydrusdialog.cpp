#include "hydrusdialog.h"

#include "flowlayout.h"
#include "remotesource.h"
#include "settings.h"
#include "uicons.h"

#include <QApplication>
#include <QClipboard>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>
#include <QtConcurrent>

namespace {

bool isLoopback(const QString &url) {
    const QString host = QUrl(url).host().toLower();
    return host == QStringLiteral("localhost") ||
           host == QStringLiteral("127.0.0.1") || host == QStringLiteral("::1") ||
           host == QStringLiteral("[::1]");
}

} // namespace

Hydrus::Config hydrusConfigFromFields(const QString &baseUrl,
                                      const QString &accessKey,
                                      const QString &proxySpec) {
    Hydrus::Config config;
    config.enabled = true;
    config.baseUrl = baseUrl.trimmed();
    if (config.baseUrl.isEmpty())
        config.baseUrl = QStringLiteral("http://127.0.0.1:45869");
    if (!config.baseUrl.contains(QStringLiteral("://")))
        config.baseUrl.prepend(QStringLiteral("http://"));
    config.accessKey = accessKey.trimmed();
    // A local daemon must never ride a global Tor/SOCKS proxy set for the
    // boorus; an explicit Hydrus proxy still wins everywhere.
    QString spec = proxySpec.trimmed();
    if (spec.isEmpty() && !isLoopback(config.baseUrl))
        spec = RemotePosts::globalProxySpec();
    if (!spec.isEmpty())
        config.proxy = RemotePosts::proxyFromSpec(spec);
    return config;
}

Hydrus::Config hydrusConfigFromSettings(const AppSettings &settings) {
    Hydrus::Config config = hydrusConfigFromFields(
        settings.hydrusUrl, settings.hydrusAccessKey, settings.hydrusProxy);
    config.enabled = settings.hydrusEnabled;
    return config;
}

HydrusFetch::HydrusFetch(const Hydrus::Config &config, QWidget *promptParent,
                         Fallback fallback, QObject *parent)
    : QObject(parent), m_config(config), m_prompt(promptParent),
      m_fallback(fallback), m_client(new Hydrus::Client(config, this)) {
    connect(m_client, &Hydrus::Client::found, this,
            [this](const Hydrus::FileTags &file, Hydrus::HashKind kind) {
                succeed(file, kind);
            });
    connect(m_client, &Hydrus::Client::notFound, this,
            [this](Hydrus::HashKind kind) { onNotFound(kind); });
    connect(m_client, &Hydrus::Client::error, this,
            [this](const QString &message) { fail(message); });
}

HydrusFetch::~HydrusFetch() { cancel(); }

void HydrusFetch::startFile(const QString &absFile) {
    cancel();
    if (!m_config.enabled) {
        emit error(tr("Hydrus is disabled in Preferences → Hydrus."));
        emit finished();
        return;
    }
    m_running = true;
    m_hashes = {};
    m_next = 0;
    m_order = {Hydrus::HashKind::Sha256};
    if (m_fallback != Fallback::None) {
        m_order << Hydrus::HashKind::Sha1 << Hydrus::HashKind::Md5;
    }
    const QString name = QFileInfo(absFile).fileName();
    emit status(tr("Hashing %1…").arg(name));
    m_hashWatcher = new QFutureWatcher<FileContentHashes>(this);
    connect(m_hashWatcher, &QFutureWatcher<FileContentHashes>::finished, this,
            [this, absFile] {
                QFutureWatcher<FileContentHashes> *watcher = m_hashWatcher;
                m_hashWatcher = nullptr;
                const FileContentHashes hashes = watcher->result();
                watcher->deleteLater();
                if (!m_running)
                    return;
                m_hashes = hashes;
                if (!hashes.ok || hashes.sha256.isEmpty()) {
                    fail(tr("Cannot read %1").arg(absFile));
                    return;
                }
                startNext();
            });
    m_hashWatcher->setFuture(QtConcurrent::run(
        [absFile] { return fileContentHashes(absFile); }));
}

void HydrusFetch::startHash(const QString &hashHex, Hydrus::HashKind kind) {
    cancel();
    if (!m_config.enabled) {
        emit error(tr("Hydrus is disabled in Preferences → Hydrus."));
        emit finished();
        return;
    }
    const QString hash = hashHex.trimmed();
    if (hash.isEmpty()) {
        emit error(tr("No content hash to look up."));
        emit finished();
        return;
    }
    m_running = true;
    m_hashes = {};
    m_hashes.ok = true;
    switch (kind) {
    case Hydrus::HashKind::Sha1:
        m_hashes.sha1 = hash;
        break;
    case Hydrus::HashKind::Md5:
        m_hashes.md5 = hash;
        break;
    case Hydrus::HashKind::Sha256:
    default:
        m_hashes.sha256 = hash;
        break;
    }
    m_order = {kind}; // only one digest is known here
    m_next = 0;
    startNext();
}

void HydrusFetch::cancel() {
    m_running = false;
    if (m_hashWatcher) {
        disconnect(m_hashWatcher, nullptr, this, nullptr);
        m_hashWatcher->deleteLater();
        m_hashWatcher = nullptr;
    }
    if (m_client)
        m_client->cancel();
}

void HydrusFetch::startNext() {
    if (!m_running)
        return;
    while (m_next < m_order.size() &&
           Hydrus::hashFor(m_order.at(m_next), m_hashes).isEmpty())
        ++m_next;
    if (m_next >= m_order.size()) {
        miss();
        return;
    }
    const Hydrus::HashKind kind = m_order.at(m_next);
    emit status(tr("Querying Hydrus (%1)…")
                    .arg(Hydrus::hashKindLabel(kind)));
    m_client->lookup(Hydrus::hashFor(kind, m_hashes), kind);
}

void HydrusFetch::onNotFound(Hydrus::HashKind kind) {
    if (!m_running)
        return;
    const bool canFallBack = m_next + 1 < m_order.size();
    if (!canFallBack || m_fallback == Fallback::None) {
        miss();
        return;
    }
    if (m_fallback == Fallback::Ask) {
        const Hydrus::HashKind next = m_order.at(m_next + 1);
        const auto answer = QMessageBox::question(
            m_prompt, tr("Hydrus tags"),
            tr("Hydrus has no tags for this file by %1.\n\nTry the legacy "
               "%2 hash? (Old hydrus databases kept SHA-1/MD5 indexes for a "
               "while; modern mirrors almost never match them.)")
                .arg(Hydrus::hashKindLabel(kind),
                     Hydrus::hashKindLabel(next)),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
        if (answer != QMessageBox::Yes) {
            miss();
            return;
        }
    }
    ++m_next;
    startNext();
}

void HydrusFetch::succeed(const Hydrus::FileTags &file, Hydrus::HashKind kind) {
    if (!m_running)
        return;
    m_running = false;
    emit found(file, kind);
    emit finished();
}

void HydrusFetch::fail(const QString &message) {
    if (!m_running)
        return;
    m_running = false;
    emit error(message);
    emit finished();
}

void HydrusFetch::miss() {
    if (!m_running)
        return;
    m_running = false;
    emit notFound();
    emit finished();
}

HydrusTagsDialog::HydrusTagsDialog(const Hydrus::Config &config,
                                   const QString &label,
                                   const QString &localFile,
                                   const QString &knownMd5, QWidget *parent)
    : QDialog(parent) {
    setWindowTitle(tr("Hydrus tags - %1").arg(label));
    resize(560, 460);
    auto *outer = new QVBoxLayout(this);
    m_status = new QLabel(tr("Looking up Hydrus tags…"), this);
    m_status->setWordWrap(true);
    m_status->setStyleSheet(QStringLiteral("color:#4a5068;"));
    outer->addWidget(m_status);

    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setBackgroundRole(QPalette::Window);
    scroll->viewport()->setBackgroundRole(QPalette::Window);
    m_body = new QWidget(scroll);
    m_bodyLayout = new QVBoxLayout(m_body);
    m_bodyLayout->setContentsMargins(0, 0, 8, 8);
    m_bodyLayout->setSpacing(6);
    scroll->setWidget(m_body);
    outer->addWidget(scroll, 1);

    m_info = new QLabel(this);
    m_info->setWordWrap(true);
    m_info->setStyleSheet(QStringLiteral("color:#4a5068;font-size:10px;"));
    outer->addWidget(m_info);

    auto *row = new QHBoxLayout();
    m_copy = new QPushButton(tr("Copy all tags"), this);
    m_copy->setEnabled(false);
    connect(m_copy, &QPushButton::clicked, this, [this] {
        if (!m_allTags.isEmpty())
            QApplication::clipboard()->setText(m_allTags.join(u' '));
    });
    auto *close = new QPushButton(tr("Close"), this);
    close->setDefault(true);
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    row->addWidget(m_copy);
    row->addStretch(1);
    row->addWidget(close);
    outer->addLayout(row);

    m_fetch = new HydrusFetch(config, this, HydrusFetch::Fallback::Silent,
                             this);
    connect(m_fetch, &HydrusFetch::status, this, [this](const QString &text) {
        m_status->setStyleSheet(QStringLiteral("color:#4a5068;"));
        m_status->setText(text);
    });
    connect(m_fetch, &HydrusFetch::found, this,
            [this](const Hydrus::FileTags &file, Hydrus::HashKind kind) {
                showFound(file, kind);
            });
    connect(m_fetch, &HydrusFetch::notFound, this, [this] {
        m_status->setStyleSheet(QStringLiteral("color:#4a5068;"));
        if (m_md5Only) {
            m_status->setText(tr("No Hydrus tags found for this post's MD5."));
            m_info->setText(
                tr("Hydrus indexes files by SHA-256; download the post to "
                   "look it up by content."));
        } else {
            m_status->setText(tr("No Hydrus tags found for this file."));
            m_info->setText(tr("Tried SHA-256, SHA-1 and MD5."));
        }
    });
    connect(m_fetch, &HydrusFetch::error, this, [this](const QString &message) {
        m_status->setStyleSheet(QStringLiteral("color:#c0392b;"));
        m_status->setText(message);
    });
    connect(m_fetch, &HydrusFetch::finished, this, [this] {
        if (m_fetch) {
            m_fetch->deleteLater();
            m_fetch = nullptr;
        }
    });

    if (!localFile.isEmpty())
        m_fetch->startFile(localFile);
    else if (!knownMd5.trimmed().isEmpty()) {
        m_md5Only = true;
        m_fetch->startHash(knownMd5, Hydrus::HashKind::Md5);
    } else
        m_status->setText(
            tr("This post has no local file and no content hash to look up."));
}

void HydrusTagsDialog::clearBody() {
    m_allTags.clear();
    m_copy->setEnabled(false);
    while (QLayoutItem *item = m_bodyLayout->takeAt(0)) {
        if (QWidget *w = item->widget()) {
            w->hide();
            w->deleteLater();
        }
        delete item;
    }
}

void HydrusTagsDialog::addGroup(const QString &name, const QString &bucket,
                                const QStringList &tags) {
    if (tags.isEmpty())
        return;
    const QString color = UiIcons::tagBucketColor(bucket).name();
    auto *head = new QLabel(QStringLiteral("%1 (%2)")
                                .arg(name)
                                .arg(tags.size()),
                            m_body);
    head->setStyleSheet(QStringLiteral("font-weight:bold;color:%1;").arg(color));
    m_bodyLayout->addWidget(head);
    auto *chipHost = new QWidget(m_body);
    auto *flow = new FlowLayout(chipHost, 2, 4, 4);
    for (const QString &tag : tags) {
        auto *chip = new QLabel(tag, chipHost);
        chip->setTextInteractionFlags(Qt::TextSelectableByMouse);
        chip->setStyleSheet(
            QStringLiteral("QLabel{color:%1;background:rgba(127,127,127,34);"
                           "border-radius:6px;padding:2px 6px;}")
                .arg(color));
        flow->addWidget(chip);
    }
    m_bodyLayout->addWidget(chipHost);
}

void HydrusTagsDialog::showFound(const Hydrus::FileTags &file,
                                 Hydrus::HashKind kind) {
    clearBody();
    m_status->setStyleSheet(QStringLiteral("color:#4a5068;"));
    const Hydrus::CategorizedTags cats = Hydrus::categorize(file.tags);
    addGroup(tr("Artist"), QStringLiteral("artist"), cats.artist);
    addGroup(tr("Character"), QStringLiteral("character"), cats.character);
    addGroup(tr("Copyright"), QStringLiteral("copyright"), cats.copyright);
    addGroup(tr("Meta"), QStringLiteral("meta"), cats.meta);
    addGroup(tr("General"), QStringLiteral("general"), cats.general);
    if (!cats.rating.isEmpty()) {
        auto *rating = new QLabel(
            tr("Hydrus rating: %1").arg(cats.rating), m_body);
        rating->setStyleSheet(
            QStringLiteral("color:#4a5068;font-size:11px;margin-top:4px;"));
        m_bodyLayout->addWidget(rating);
    }
    m_allTags.clear();
    for (const QStringList *bucket :
         {&cats.artist, &cats.character, &cats.copyright, &cats.meta,
          &cats.general}) {
        for (const QString &tag : *bucket) {
            if (!m_allTags.contains(tag))
                m_allTags.append(tag);
        }
    }
    m_copy->setEnabled(!m_allTags.isEmpty());
    if (file.tags.isEmpty())
        m_status->setText(
            tr("Hydrus knows this file but has no tags for it."));
    else
        m_status->setText(
            tr("%1 tag(s) from Hydrus").arg(file.tags.size()));
    m_bodyLayout->addStretch(1);
    QString info = tr("Matched by %1").arg(Hydrus::hashKindLabel(kind));
    if (file.fileId > 0)
        info += tr(" · Hydrus file #%1").arg(file.fileId);
    m_info->setText(info);
}
