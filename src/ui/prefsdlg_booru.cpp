#include "prefsdlg.h"

#include "faviconcache.h"
#include "remotesource.h"
#include "settings.h"
#include "uicons.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTimer>
#include <QEvent>
#include <QEventLoop>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QMouseEvent>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QtConcurrent>
#include <QUrl>

void PrefsDialog::commitBooruEditor(const QString &key) {
    if (key.isEmpty() || m_loadingBooru)
        return;
    for (BooruSite &b : m_booruEdits) {
        if (b.key != key)
            continue;
        b.label = m_booruName->text().trimmed();
        if (b.label.isEmpty())
            b.label = b.key;
        b.enabled = m_booruEnabled->isChecked();
        b.shortName = m_booruShort->text().trimmed();
        if (b.shortName.isEmpty())
            b.shortName = RemotePosts::shortFor(b.label);
        b.badgeColor = m_booruColor.name();
        QString base = m_booruBase->text().trimmed();
        while (base.endsWith(u'/'))
            base.chop(1);
        const QString oldHost = QUrl(b.baseUrl).host().toLower();
        const QString newHost = QUrl(base).host().toLower();
        b.baseUrl = base;
        b.engine = RemotePosts::engineForKey(m_booruEngine->currentData().toString());
        b.user = m_booruUser->text().trimmed();
        b.secret = m_booruSecret->text();
        b.skipAutoClassify = m_booruNoClassify->isChecked();
        b.localBlacklist = m_booruLocalBlacklist->isChecked();
        if (m_booruCacheInherit->isChecked())
            b.cacheMode = CacheMode::Inherit;
        else if (m_booruCacheNone->isChecked())
            b.cacheMode = CacheMode::None;
        else if (m_booruCacheSfw->isChecked())
            b.cacheMode = CacheMode::SfwOnly;
        else
            b.cacheMode = CacheMode::All;
        b.defaultTags = m_booruTags->text().trimmed();
        if (m_booruDragMode) {
            const int dm = m_booruDragMode->currentData().toInt();
            b.dragLinkMode =
                dm == int(DragLinkMode::DirectMedia)
                    ? DragLinkMode::DirectMedia
                    : (dm == int(DragLinkMode::PostPage)
                           ? DragLinkMode::PostPage
                           : DragLinkMode::Inherit);
        }
        b.proxy = m_booruProxy->text().trimmed();
        // Fresh entries still wearing the generic blue inherit the new
        // host's house color (explicit picks and existing entries are
        // untouched: this only fires while the color is still default).
        if (b.badgeColor.compare(QStringLiteral("#3a7ec2"),
                                 Qt::CaseInsensitive) == 0 &&
            QRegularExpression(QStringLiteral("^newbooru\\d*$"))
                .match(b.key)
                .hasMatch())
            b.badgeColor = RemotePosts::defaultBadgeColor(b.baseUrl, b.engine);
        // Seed per-host default cookies once (quirks table: gelbooru.com
        // wants fringeBenefits=yup for restricted content). They land in
        // the stored list, so they stay visible - and removable - in the
        // cookie manager. Keyed on the host transition so a deliberate
        // removal is never re-added behind your back.
        if (oldHost != newHost)
            b.cookies << RemotePosts::seedQuirkCookies(b.cookies, newHost);
        // Fresh "newbooru" entries get a descriptive key the first time
        // they are named or pointed at a host ("Rule34" -> rule34,
        // https://rule34.xxx -> rule34). After that the key is frozen:
        // saved tabs and defaults reference it.
        if (QRegularExpression(QStringLiteral("^newbooru\\d*$"))
                .match(b.key)
                .hasMatch()) {
            QSet<QString> taken;
            for (const BooruSite &o : m_booruEdits) {
                if (o.key != b.key)
                    taken.insert(o.key);
            }
            QString candidate;
            if (b.label.compare(QStringLiteral("New booru"),
                               Qt::CaseInsensitive) != 0)
                candidate = RemotePosts::uniqueBooruKey(b.label, taken);
            else if (!newHost.isEmpty() && newHost != QStringLiteral("www"))
                candidate = RemotePosts::uniqueBooruKey(
                    newHost.split(u'.').first(), taken);
            else if (!newHost.isEmpty())
                candidate = RemotePosts::uniqueBooruKey(newHost, taken);
            if (!candidate.isEmpty() && candidate != b.key) {
                const QString oldKey = b.key;
                b.key = candidate;
                // A staged favicon follows the rename.
                if (m_pendingFavicons.contains(oldKey))
                    m_pendingFavicons.insert(
                        candidate, m_pendingFavicons.take(oldKey));
                // Keep the list + defaults picker pointing at the entry.
                for (int i = 0; i < m_booruList->count(); ++i) {
                    QListWidgetItem *item = m_booruList->item(i);
                    if (item->data(Qt::UserRole).toString() == oldKey) {
                        item->setData(Qt::UserRole, candidate);
                        item->setText(b.label);
                    }
                }
                if (m_defBoorus) {
                    for (int i = 0; i < m_defBoorus->count(); ++i) {
                        QListWidgetItem *item = m_defBoorus->item(i);
                        if (item->data(Qt::UserRole).toString() == oldKey)
                            item->setData(Qt::UserRole, candidate);
                    }
                }
            }
        }
        // Live list title: the left pane follows every keystroke, not
        // just the one-time key rename above (which is why the name
        // used to freeze after its first letter).
        for (int i = 0; i < m_booruList->count(); ++i) {
            QListWidgetItem *item = m_booruList->item(i);
            if (item->data(Qt::UserRole).toString() == b.key) {
                item->setText(b.label);
                break;
            }
        }
        return;
    }
}

void PrefsDialog::saveBooruEditor() {
    BooruSite *e = currentBooruEntry();
    if (e)
        commitBooruEditor(e->key);
}

BooruSite *PrefsDialog::currentBooruEntry() {
    const int row = m_booruList ? m_booruList->currentRow() : -1;
    if (row < 0 || !m_booruList)
        return nullptr;
    const QListWidgetItem *item = m_booruList->item(row);
    const QString key = item ? item->data(Qt::UserRole).toString() : QString();
    if (key.isEmpty())
        return nullptr;
    for (BooruSite &b : m_booruEdits) {
        if (b.key == key)
            return &b;
    }
    return nullptr;
}

void PrefsDialog::updateCookieLabel() {
    if (!m_booruCookieLabel)
        return;
    const BooruSite *e = currentBooruEntry();
    const int n = e ? e->cookies.size() : 0;
    m_booruCookieLabel->setText(
        n == 0 ? tr("none") : tr("%1 set").arg(n));
}

QString PrefsDialog::customFaviconDir() const {
    QString dataDir = m_dataDir ? m_dataDir->text().trimmed() : QString();
    if (dataDir.isEmpty())
        dataDir = defaultDataDir();
    return dataDir + QStringLiteral("/favicons");
}

void PrefsDialog::updateFaviconPreview() {
    if (!m_booruFavicon)
        return;
    const BooruSite *e = currentBooruEntry();
    QPixmap pm;
    QString tip;
    if (e) {
        // Staged choice wins, then the saved custom file, then the
        // auto-fetched icon (if the cache already holds one).
        QString src = m_pendingFavicons.value(e->key);
        if (src.isEmpty() && !m_pendingFavicons.contains(e->key))
            src = customFaviconDir() + u'/' + e->key +
                  QStringLiteral(".png");
        if (!src.isEmpty()) {
            pm = QIcon(src).pixmap(16, 16);
            tip = src;
        }
        if (pm.isNull() && m_favicons) {
            const QIcon autoIcon = m_favicons->iconFor(e->key, e->baseUrl);
            if (!autoIcon.isNull()) {
                pm = autoIcon.pixmap(16, 16);
                tip = tr("Auto-fetched");
            }
        }
    }
    if (pm.isNull()) {
        m_booruFavicon->setText(tr("-"));
        m_booruFavicon->setToolTip(tr("No icon yet"));
    } else {
        m_booruFavicon->setText(QString());
        m_booruFavicon->setPixmap(pm);
        m_booruFavicon->setToolTip(tip);
    }
}

void PrefsDialog::loadBooruEditor(int row) {
    const BooruSite *found = nullptr;
    if (row >= 0) {
        const QListWidgetItem *item = m_booruList->item(row);
        const QString key = item ? item->data(Qt::UserRole).toString() : QString();
        for (const BooruSite &b : m_booruEdits) {
            if (b.key == key) {
                found = &b;
                break;
            }
        }
    }
    const bool on = found != nullptr;
    // Disabled controls hide entirely instead of sitting greyed-out.
    m_booruFormBox->setVisible(on);
    m_booruEmpty->setVisible(!on);
    // Guarded: these programmatic fills emit textChanged, which must not
    // commit half-loaded widgets back into the entry (see m_loadingBooru).
    m_loadingBooru = true;
    if (!found) {
        m_booruName->clear();
        m_booruBase->clear();
        m_booruUser->clear();
        m_booruSecret->clear();
        m_booruShort->clear();
        m_booruNoClassify->setChecked(false);
        m_booruLocalBlacklist->setChecked(false);
        m_booruEnabled->setChecked(true);
        m_booruCacheInherit->setChecked(true);
        m_booruTags->clear();
        if (m_booruDragMode)
            m_booruDragMode->setCurrentIndex(0);
        m_booruProxy->clear();
        m_booruColor = QColor(QStringLiteral("#3a7ec2"));
        setBooruSwatch();
        updateCookieLabel();
        updateFaviconPreview();
        m_loadingBooru = false;
        return;
    }
    m_booruName->setText(found->label);
    m_booruEnabled->setChecked(found->enabled);
    m_booruShort->setText(found->shortName);
    m_booruColor = QColor(found->badgeColor);
    if (!m_booruColor.isValid())
        m_booruColor = QColor(QStringLiteral("#3a7ec2"));
    setBooruSwatch();
    m_booruBase->setText(found->baseUrl);
    m_booruEngine->setCurrentIndex(
        m_booruEngine->findData(
            RemotePosts::engineKey(found->engine)));
    m_booruUser->setText(found->user);
    m_booruSecret->setText(found->secret);
    m_booruNoClassify->setChecked(found->skipAutoClassify);
    m_booruLocalBlacklist->setChecked(found->localBlacklist);
    switch (found->cacheMode) {
    case CacheMode::All:
        m_booruCacheAll->setChecked(true);
        break;
    case CacheMode::SfwOnly:
        m_booruCacheSfw->setChecked(true);
        break;
    case CacheMode::None:
        m_booruCacheNone->setChecked(true);
        break;
    case CacheMode::Inherit:
    default:
        m_booruCacheInherit->setChecked(true);
        break;
    }
    m_booruTags->setText(found->defaultTags);
    if (m_booruDragMode)
        m_booruDragMode->setCurrentIndex(qMax(
            0, m_booruDragMode->findData(int(found->dragLinkMode))));
    m_booruProxy->setText(found->proxy);
    updateCookieLabel();
    updateFaviconPreview();
    m_loadingBooru = false;
}

void PrefsDialog::refreshBooruList() {
    m_booruList->blockSignals(true);
    m_booruList->clear();
    for (const BooruSite &b : m_booruEdits) {
        auto *item = new QListWidgetItem(
            b.enabled ? b.label : tr("%1 (disabled)").arg(b.label),
            m_booruList);
        item->setData(Qt::UserRole, b.key);
        if (!b.enabled)
            item->setForeground(palette().color(QPalette::Disabled,
                                                QPalette::Text));
    }
    m_booruList->blockSignals(false);
    if (m_booruList->count() > 0 && m_booruList->currentRow() < 0)
        m_booruList->setCurrentRow(0);
    else
        loadBooruEditor(m_booruList->currentRow());
}

void PrefsDialog::refreshBooruListLabels() {
    for (int i = 0; i < m_booruList->count(); ++i) {
        QListWidgetItem *item = m_booruList->item(i);
        const QString key = item->data(Qt::UserRole).toString();
        for (const BooruSite &b : m_booruEdits) {
            if (b.key == key) {
                item->setText(b.enabled
                                  ? b.label
                                  : tr("%1 (disabled)").arg(b.label));
                item->setForeground(
                    b.enabled
                        ? palette().color(QPalette::Text)
                        : palette().color(QPalette::Disabled,
                                          QPalette::Text));
                break;
            }
        }
    }
}

void PrefsDialog::harvest() {
    harvestInto(m_result);
    commitPendingFavicons();
}

// Custom favicons: convert staged choices to
// <dataDir>/favicons/<key>.png, drop files for removed entries (and
// staged removals). Runs only on real Apply/OK - harvestInto doubles
// as the Apply-state comparison and must never touch files.
void PrefsDialog::commitPendingFavicons() {
    if (m_pendingFavicons.isEmpty())
        return;
    const QString favDir = effectiveDataDir(m_result) + QStringLiteral("/favicons");
    QSet<QString> live;
    for (const BooruSite &b : m_result.boorus)
        live.insert(b.key);
    QDir().mkpath(favDir);
    for (auto it = m_pendingFavicons.begin();
         it != m_pendingFavicons.end(); ++it) {
        const QString dest = favDir + u'/' + it.key() +
                             QStringLiteral(".png");
        if (it.value().isEmpty()) {
            QFile::remove(dest);
            continue;
        }
        QImage img(it.value());
        if (img.isNull()) {
            m_accStatus->setText(
                tr("✗ favicon image unreadable: %1").arg(it.value()));
            continue;
        }
        if (!img.scaled(32, 32, Qt::KeepAspectRatio,
                        Qt::SmoothTransformation)
                 .save(dest, "PNG")) {
            m_accStatus->setText(
                tr("✗ cannot write favicon: %1").arg(dest));
        }
    }
    m_pendingFavicons.clear();
    QDir dir(favDir);
    for (const QString &f :
         dir.entryList({QStringLiteral("*.png")}, QDir::Files)) {
        if (!live.contains(f.left(f.size() - 4)))
            QFile::remove(favDir + u'/' + f);
    }
    updateFaviconPreview();
}

void PrefsDialog::updateApplyState() {
    if (!m_applyBtn)
        return;
    AppSettings cur = m_result;
    harvestInto(cur);
    // Staged favicons live outside AppSettings: any pending choice or
    // removal is itself an unapplied change.
    m_applyBtn->setEnabled(cur != m_initial || !m_pendingFavicons.isEmpty());
}


namespace {
// Blocking engine probe (worker thread): fetch shape-revealing endpoints,
// first match wins. -1 when nothing matches (walled/gone/unknown).
int detectEngineFor(const QString &base) {
    QNetworkAccessManager nam;
    auto get = [&](const QString &url) -> QByteArray {
        QNetworkRequest req{QUrl(url)};
        req.setTransferTimeout(10000);
        req.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("box/0.1 (engine detect)"));
        req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
        QNetworkReply *reply = nam.get(req);
        QEventLoop loop;
        QTimer timer;
        timer.setSingleShot(true);
        QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
        QObject::connect(reply, &QNetworkReply::finished, &loop,
                         &QEventLoop::quit);
        timer.start(12000);
        loop.exec();
        timer.stop();
        QByteArray body;
        if (reply->error() == QNetworkReply::NoError)
            body = reply->readAll();
        reply->deleteLater();
        return body;
    };
    auto firstObj = [](const QByteArray &body) -> QJsonObject {
        QJsonParseError err;
        const QJsonDocument doc = QJsonDocument::fromJson(body, &err);
        if (err.error != QJsonParseError::NoError || !doc.isArray() ||
            doc.array().isEmpty())
            return {};
        const QJsonValue v = doc.array().first();
        return v.isObject() ? v.toObject() : QJsonObject();
    };
    // Shimmie: XML only (danbooru_api ext).
    {
        const QByteArray xml =
            get(base + QStringLiteral("/api/danbooru/post/index.xml?limit=1"));
        if (xml.trimmed().startsWith('<') && xml.contains("<posts"))
            return int(BooruEngine::Shimmie);
    }
    // Szurubooru: version endpoint, else results-shaped listing.
    {
        QJsonParseError err;
        const QJsonDocument info = QJsonDocument::fromJson(
            get(base + QStringLiteral("/api/info")), &err);
        if (err.error == QJsonParseError::NoError && info.isObject() &&
            (info.object().contains(QStringLiteral("serverTime")) ||
             info.object().contains(QStringLiteral("version"))))
            return int(BooruEngine::Szurubooru);
        const QJsonDocument list = QJsonDocument::fromJson(
            get(base + QStringLiteral("/api/posts/?limit=1")), &err);
        if (err.error == QJsonParseError::NoError && list.isObject() &&
            list.object().value(QStringLiteral("results")).isArray())
            return int(BooruEngine::Szurubooru);
    }
    // Danbooru: per-bucket tag_string_* fields.
    {
        const QJsonObject o =
            firstObj(get(base + QStringLiteral("/posts.json?limit=1")));
        if (!o.isEmpty() &&
            (o.contains(QStringLiteral("tag_string_artist")) ||
             o.contains(QStringLiteral("tag_string_general")) ||
             o.contains(QStringLiteral("tag_string_copyright"))))
            return int(BooruEngine::Danbooru);
    }
    // Moebooru: flat "tags" string (epoch dates where present).
    // Gelbooru never serves /post.json, so anything tags-shaped here
    // is moebooru-family.
    {
        const QJsonObject o =
            firstObj(get(base + QStringLiteral("/post.json?limit=1")));
        if (!o.isEmpty() && o.value(QStringLiteral("tags")).isString())
            return int(BooruEngine::Moebooru);
    }
    // Gelbooru dapi family: {"post": [...]} or a bare tags array.
    {
        const QByteArray body = get(
            base +
            QStringLiteral("/index.php?page=dapi&s=post&q=index&limit=1&json=1"));
        QJsonParseError err;
        const QJsonDocument doc = QJsonDocument::fromJson(body, &err);
        if (err.error == QJsonParseError::NoError) {
            if (doc.isObject() &&
                doc.object().value(QStringLiteral("post")).isArray())
                return int(BooruEngine::Gelbooru);
            if (doc.isArray() && !doc.array().isEmpty()) {
                const QJsonValue v = doc.array().first();
                if (v.isObject() &&
                    v.toObject().value(QStringLiteral("tags")).isString())
                    return int(BooruEngine::Gelbooru);
            }
        }
    }
    return -1;
}
} // namespace

void PrefsDialog::detectBooruEngine(QPushButton *detectBtn) {
    saveBooruEditor();
    BooruSite *e = currentBooruEntry();
    if (!e || e->baseUrl.isEmpty()) {
        m_accStatus->setText(tr("✗ set an instance URL first"));
        return;
    }
    detectBtn->setEnabled(false);
    m_accStatus->setText(tr("Detecting engine…"));
    const QString base = e->baseUrl;
    QFutureWatcher<int> *watcher = new QFutureWatcher<int>(this);
    connect(watcher, &QFutureWatcher<int>::finished, this,
            [this, watcher, detectBtn] {
                const int found = watcher->result();
                detectBtn->setEnabled(true);
                watcher->deleteLater();
                if (found < 0) {
                    m_accStatus->setText(
                        tr("✗ engine not recognized - pick it manually"));
                    return;
                }
                const BooruEngine engine = static_cast<BooruEngine>(found);
                m_booruEngine->setCurrentIndex(qMax(
                    0, m_booruEngine->findData(
                           RemotePosts::engineKey(engine))));
                saveBooruEditor();
                updateApplyState();
                m_accStatus->setText(
                    tr("✓ detected: %1")
                        .arg(RemotePosts::engineLabel(engine)));
            });
    watcher->setFuture(QtConcurrent::run([base] {
        return detectEngineFor(base);
    }));
}

void PrefsDialog::testSite(const BooruSite &probed) {
    if (probed.baseUrl.isEmpty()) {
        m_accStatus->setText(
            tr("✗ %1 needs an instance URL first").arg(probed.label));
        return;
    }
    m_accStatus->setText(tr("Testing %1…").arg(probed.label));
    m_accStatus->setCursor(Qt::ArrowCursor);
    m_accStatus->setToolTip(QString());
    m_lastProbeValid = false;
    const SiteCreds creds{probed.user, probed.secret};
    // Probing blocks with its own loop; run it off the UI thread so the
    // dialog stays alive, then report back.
    const BooruSite site = probed;
    qRegisterMetaType<RemoteSource::CredentialProbe>();
    QFutureWatcher<RemoteSource::CredentialProbe> *watcher =
        new QFutureWatcher<RemoteSource::CredentialProbe>(this);
    connect(watcher, &QFutureWatcher<RemoteSource::CredentialProbe>::finished,
            this, [this, watcher] {
                m_lastProbe = watcher->result();
                m_lastProbeValid = true;
                m_accStatus->setCursor(Qt::PointingHandCursor);
                m_accStatus->setToolTip(
                    tr("Click to see the full server response"));
                const QString label = m_lastProbe.siteLabel;
                const bool ok = m_lastProbe.ok;
                QString msg = m_lastProbe.message;
                // Server error bodies can be huge unbroken blobs: cap the
                // line so even unwrapped it cannot stretch the dialog.
                static constexpr int kMaxMsg = 240;
                if (msg.size() > kMaxMsg)
                    msg = msg.left(kMaxMsg) + QStringLiteral("…");
                m_accStatus->setText(
                    tr("%1 %2: %3").arg(ok ? QStringLiteral("✓")
                                           : QStringLiteral("✗"),
                                      label, msg));
                watcher->deleteLater();
            });
    watcher->setFuture(QtConcurrent::run([site, creds] {
        return RemoteSource::probeCredentials(site, creds);
    }));
}

bool PrefsDialog::eventFilter(QObject *o, QEvent *e) {
    if (o == m_accStatus && e->type() == QEvent::MouseButtonPress &&
        m_lastProbeValid) {
        showProbeDetails();
        return true;
    }
    return QDialog::eventFilter(o, e);
}

void PrefsDialog::showProbeDetails() {
    if (!m_lastProbeValid)
        return;
    QString cookies =
        m_lastProbe.sentCookieNames.isEmpty()
            ? tr("(none)")
            : m_lastProbe.sentCookieNames.join(QStringLiteral(", "));
    RemotePosts::showProbeDialog(
        this, tr("Server response - %1").arg(m_lastProbe.siteLabel),
        RemotePosts::probeReportText(
            RemotePosts::redactSecrets(
                m_lastProbe.url.toString(QUrl::FullyEncoded)),
            m_lastProbe.http, m_lastProbe.body) +
            tr("\nCookies sent: %1").arg(cookies));
}


