#include "cookiegrab.h"

#include "remotesource.h" // normalizeCookies

#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QMap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <QWebEngineView>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineCookieStore>
#include <QWebEngineDownloadRequest>

// ── Manual editor ────────────────────────────────────────────────

CookieEditorDialog::CookieEditorDialog(const QStringList &cookies,
                                       QWidget *parent)
    : QDialog(parent) {
    setWindowTitle(tr("Edit cookies"));
    resize(480, 320);
    auto *lay = new QVBoxLayout(this);
    auto *note = new QLabel(
        tr("One <b>name=value</b> per line. Pasting a whole "
           "<b>a=1; b=2</b> header (or Set-Cookie lines) works too — "
           "attributes are dropped, garbage is ignored."),
        this);
    note->setWordWrap(true);
    lay->addWidget(note);
    m_edit = new QPlainTextEdit(this);
    m_edit->setPlainText(cookies.join(u'\n'));
    m_edit->setTabChangesFocus(true);
    lay->addWidget(m_edit, 1);
    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        harvest();
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    lay->addWidget(buttons);
}

void CookieEditorDialog::harvest() {
    m_cookies = RemotePosts::normalizeCookies(m_edit->toPlainText().split(u'\n'));
}

// ── Browser capture ────────────────────────────────────────────

namespace {
// One shared profile per storage path (parented to the application, not
// the dialog): creating a profile per capture re-ran WebEngine init on
// every open (the visible prefs flicker) and raced teardown against the
// next open fighting over the same on-disk jar.
QMap<QString, QWebEngineProfile *> &captureProfiles() {
    static QMap<QString, QWebEngineProfile *> profiles;
    return profiles;
}

QWebEngineProfile *sharedCaptureProfile(const QString &profilePath) {
    if (QWebEngineProfile *existing = captureProfiles().value(profilePath))
        return existing;
    auto *profile = new QWebEngineProfile(
        QStringLiteral("weblogin"), QCoreApplication::instance());
    // Fingerprint honesty: advertise the engine's REAL Chromium version
    // (a stale pinned UA + modern TLS fingerprint reads as a bot and
    // Turnstile loops forever). API/media requests deliberately do NOT
    // reuse this (see captureUserAgent): they identify honestly and
    // carry harvested clearance in the Cookie header instead.
    RemotePosts::setCaptureUserAgent(profile->httpUserAgent());
    profile->setHttpUserAgent(RemotePosts::captureUserAgent());
    if (!profilePath.isEmpty()) {
        QDir().mkpath(profilePath);
        profile->setPersistentStoragePath(profilePath);
    }
    profile->setPersistentCookiesPolicy(
        QWebEngineProfile::AllowPersistentCookies);
    // Login reuse needs the cookie jar on disk, but a persistent HTTP
    // cache only buys a multi-MB flush that blocks the GUI thread on
    // close (the hang behind the "frozen settings" reports).
    profile->setHttpCacheType(QWebEngineProfile::MemoryHttpCache);
    profile->setHttpCacheMaximumSize(50 * 1024 * 1024);
    captureProfiles().insert(profilePath, profile);
    return profile;
}
} // namespace

CookieGrabDialog::CookieGrabDialog(const QString &siteLabel,
                                   const QString &baseUrl,
                                   const QString &profilePath,
                                   QWidget *parent)
    : QDialog(parent), m_host(QUrl(baseUrl).host().toLower()) {
    setWindowTitle(tr("Capture cookies — %1").arg(siteLabel));
    resize(960, 680);
    // Qt6 initializes WebEngine on first use; no manual init needed.
    auto *lay = new QVBoxLayout(this);
    auto *note = new QLabel(
        tr("Log in and solve any captcha/verification challenge in the "
           "page below. A solved challenge collects the cookies but the "
           "window stays open — verify the page, then press <b>Use these "
           "cookies</b> to save. The login itself stays in a separate "
           "capture profile; only the harvested <b>name=value</b> pairs "
           "are kept with this booru."),
        this);
    note->setWordWrap(true);
    lay->addWidget(note);
    // Shared profile: no per-open init flash, no teardown races, and the
    // login (but no other state) survives between captures.
    m_profile = sharedCaptureProfile(profilePath);
    connect(m_profile->cookieStore(), &QWebEngineCookieStore::cookieAdded,
            this, &CookieGrabDialog::onCookieAdded);
    // The page hangs off the view so teardown order is deterministic:
    // view (and page) die before the profile, never the reverse.
    m_view = new QWebEngineView(this);
    m_page = new QWebEnginePage(m_profile, m_view);
    connect(m_page, &QWebEnginePage::urlChanged, this,
            &CookieGrabDialog::onUrlChanged);
    connect(m_page, &QWebEnginePage::loadFinished, this,
            &CookieGrabDialog::onLoadFinished);
    m_view->setPage(m_page);
    lay->addWidget(m_view, 1);
    auto *statRow = new QHBoxLayout();
    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    m_count = new QLabel(this);
    statRow->addWidget(m_status, 1);
    statRow->addWidget(m_count);
    lay->addLayout(statRow);
    auto *buttons = new QDialogButtonBox(this);
    m_useBtn = buttons->addButton(tr("Use these cookies"),
                                  QDialogButtonBox::AcceptRole);
    connect(m_useBtn, &QPushButton::clicked, this, &CookieGrabDialog::useCookies);
    auto *closeBtn = buttons->addButton(QDialogButtonBox::Close);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::reject);
    lay->addWidget(buttons);
    setStatus(tr("Loading %1…").arg(baseUrl));
    m_view->load(QUrl(baseUrl));
    // The persistent jar replays its cookies as added-events on startup.
    // Only auto-save on challenges solved *in this capture*, not on a
    // restored login (which would self-accept the moment the dialog opens).
    QTimer::singleShot(3000, this, [this] {
        m_armed = true;
        if (m_restoredClearance && m_cookies.isEmpty())
            setStatus(tr("Existing login found — press <b>Use these "
                         "cookies</b> if it is still valid, or log in "
                         "again to refresh it."));
    });
}

CookieGrabDialog::~CookieGrabDialog() {
    // Explicit view-first teardown: the renderer detaches while the
    // profile (and its cookie store) is still alive. Relying on
    // implicit child order here hung/crashed on challenge-heavy pages.
    if (m_view) {
        m_view->stop();
        delete m_view;
        m_view = nullptr;
        m_page = nullptr; // was parented to the view
    }
}

void CookieGrabDialog::setStatus(const QString &text) {
    m_status->setText(text);
}

namespace {
bool cookieBelongsTo(const QNetworkCookie &cookie, const QString &host) {
    if (host.isEmpty() || cookie.name().isEmpty())
        return false;
    QString domain = cookie.domain().toLower().trimmed();
    if (domain.startsWith(u'.'))
        domain = domain.mid(1);
    if (domain.isEmpty())
        return true; // host-only session cookie: keep, server set it
    return domain == host || host.endsWith(u'.' + domain) ||
           domain.endsWith(u'.' + host);
}
} // namespace

void CookieGrabDialog::onUrlChanged(const QUrl &url) {
    setStatus(url.toString());
}

void CookieGrabDialog::onLoadFinished(bool ok) {
    if (!ok)
        setStatus(tr("Page failed to load — check the address and retry."));
}

void CookieGrabDialog::onCookieAdded(const QNetworkCookie &cookie) {
    // Challenge pages churn cookies (clearance rotation, retries):
    // keep the jar bounded, later arrivals win at harvest.
    if (m_seen.size() >= 2000)
        m_seen.removeFirst();
    m_seen.append(cookie);
    m_count->setText(tr("%1 cookies in capture profile").arg(m_seen.size()));
    if (cookie.name().isEmpty() || !cookieBelongsTo(cookie, m_host))
        return;
    const QString name = QString::fromUtf8(cookie.name()).toLower();
    // Cloudflare cookies anywhere in the flow mean a Cloudflare wall is
    // (or was) in play: only its clearance ever auto-saves after that.
    if (name == QStringLiteral("cf_clearance") || name.startsWith(QStringLiteral("cf_")) ||
        name.startsWith(QStringLiteral("__cf")))
        m_cfSeen = true;
    if (!m_armed) {
        if (name == QStringLiteral("cf_clearance"))
            m_restoredClearance = true; // pre-existing login, not a fresh solve
        return;
    }
    if (m_autoFired || m_flushing)
        return;
    // Fresh clearance for this host: a challenge was just solved (Cloudflare
    // clearance, or a proof-of-work wall like ATF's that sets its cookie on
    // success). Short consent-banner values never qualify, so those can't
    // self-accept the dialog mid-browse.
    const bool solved =
        name == QStringLiteral("cf_clearance") ||
        (!m_cfSeen && cookie.value().size() >= 16);
    if (!solved)
        return;
    // Settle briefly (redirect + follow-up cookies still landing),
    // then collect — but stay open so the result can be verified.
    m_autoFired = true;
    setStatus(tr("Challenge solved — collecting cookies…"));
    QTimer::singleShot(1500, this, &CookieGrabDialog::autoHarvest);
}

void CookieGrabDialog::autoHarvest() {
    if (m_flushing)
        return; // a manual harvest is running; it closes by itself
    m_autoAccept = false;
    useCookies();
}

void CookieGrabDialog::useCookies() {
    // Async flush: loadAllCookies() replays the jar through cookieAdded,
    // then finishHarvest() collects. The old nested QEventLoop here kept
    // pumping renderer callbacks while the dialog could close underneath —
    // a textbook use-after-free.
    if (m_flushing)
        return;
    m_flushing = true;
    if (m_useBtn)
        m_useBtn->setEnabled(false);
    setStatus(tr("Collecting cookies…"));
    m_profile->cookieStore()->loadAllCookies();
    QTimer::singleShot(1200, this, &CookieGrabDialog::finishHarvest);
}

void CookieGrabDialog::finishHarvest() {
    m_flushing = false;
    if (m_useBtn)
        m_useBtn->setEnabled(true);
    m_cookies = harvestForHost(m_seen, m_host);
    if (m_cookies.isEmpty()) {
        setStatus(tr("No cookies for this site yet — log in (or solve the "
                     "challenge) first, then try again."));
        m_autoAccept = true;
        return;
    }
    if (!m_autoAccept) {
        // Solved-challenge collection: report, stay open for verification.
        // The next manual press harvests fresh and closes as before.
        m_autoAccept = true;
        setStatus(tr("Challenge solved — %1 cookies ready. Verify the page, "
                     "then press <b>Use these cookies</b> to save.")
                      .arg(m_cookies.size()));
        return;
    }
    accept();
}

QStringList CookieGrabDialog::harvestForHost(
    const QList<QNetworkCookie> &seen, const QString &host) {
    QStringList raw;
    for (const QNetworkCookie &c : seen) {
        if (!cookieBelongsTo(c, host))
            continue;
        const QString name = QString::fromUtf8(c.name());
        const QString value = QString::fromUtf8(c.value());
        if (!name.isEmpty() && !value.isEmpty())
            raw << name + u'=' + value;
    }
    return RemotePosts::normalizeCookies(raw);
}

// Silent renewal worker: hidden view, shared profile, one page load.
// No Q_OBJECT: signal hooks are lambdas bound to the worker's lifetime.
class SilentRefresher : public QObject {
public:
    SilentRefresher(const QString &baseUrl, const QString &profilePath,
                    std::function<void(const QStringList &)> done)
        : m_host(QUrl(baseUrl).host().toLower()), m_done(std::move(done)) {
        m_profile = sharedCaptureProfile(profilePath);
        connect(m_profile->cookieStore(),
                &QWebEngineCookieStore::cookieAdded, this,
                [this](const QNetworkCookie &cookie) {
                    if (m_finished)
                        return;
                    if (m_seen.size() >= 2000)
                        m_seen.removeFirst();
                    m_seen.append(cookie);
                });
        m_view = new QWebEngineView();
        m_view->setVisible(false);
        m_page = new QWebEnginePage(m_profile, m_view);
        connect(m_page, &QWebEnginePage::loadFinished, this,
                [this](bool) {
                    if (m_finished)
                        return;
                    // Let post-load proof-of-work + redirects settle.
                    QTimer::singleShot(4000, this,
                                       [this] { harvestAndFinish(); });
                });
        m_view->setPage(m_page);
        m_view->load(QUrl(baseUrl));
        // Hard ceiling: report whatever the jar holds by then.
        QTimer::singleShot(30000, this, [this] { harvestAndFinish(); });
    }

private:
    void harvestAndFinish() {
        if (m_finished)
            return;
        m_finished = true;
        // Replay the persistent jar too, not just session arrivals, then
        // harvest on the next event-loop turn once replayed.
        m_profile->cookieStore()->loadAllCookies();
        QTimer::singleShot(1500, this, [this] {
            auto done = std::move(m_done);
            const QStringList fresh =
                CookieGrabDialog::harvestForHost(m_seen, m_host);
            // View-first teardown (same rule as the dialog destructor).
            m_page = nullptr; // parented to the view
            m_view->stop();
            m_view->deleteLater();
            m_view = nullptr;
            deleteLater();
            if (done)
                done(fresh);
        });
    }

    QString m_host;
    std::function<void(const QStringList &)> m_done;
    bool m_finished = false;
    QWebEngineProfile *m_profile = nullptr;
    QWebEngineView *m_view = nullptr;
    QWebEnginePage *m_page = nullptr;
    QList<QNetworkCookie> m_seen;
};

void CookieGrabDialog::refreshSilent(
    const QString &baseUrl, const QString &profilePath,
    std::function<void(const QStringList &)> done) {
    // Parent to the application: outlives whatever dialog triggered it.
    new SilentRefresher(baseUrl, profilePath, std::move(done));
}

WebEngineDownloadBackend::WebEngineDownloadBackend(QObject *parent)
    : WebDownloadBackend(parent) {
    // NOTE: caller (the app) constructs on the GUI thread.
    m_profile = WebApiFetcher::captureProfile();
    connect(m_profile, &QWebEngineProfile::downloadRequested, this,
            [this](QWebEngineDownloadRequest *req) {
                // Route profile downloads to the pending job with the
                // same URL (FIFO). Foreign requests (e.g. from the
                // visible capture browser sharing this profile) are
                // left alone.
                for (auto it = m_jobs.begin(); it != m_jobs.end(); ++it) {
                    if (it->req == nullptr && it->url == req->url()) {
                        it->req = req;
                        connect(req, &QWebEngineDownloadRequest::receivedBytesChanged,
                                this, [this, req] {
                                    if (Job *j = jobForRequest(req))
                                        emit progress(j->id, req->receivedBytes(),
                                                      req->totalBytes());
                                });
                        connect(req, &QWebEngineDownloadRequest::totalBytesChanged,
                                this, [this, req] {
                                    if (Job *j = jobForRequest(req))
                                        emit progress(j->id, req->receivedBytes(),
                                                      req->totalBytes());
                                });
                        connect(req, &QWebEngineDownloadRequest::stateChanged,
                                this, [this, req](QWebEngineDownloadRequest::DownloadState) {
                                    if (Job *j = jobForRequest(req)) {
                                        const auto st = req->state();
                                        if (st == QWebEngineDownloadRequest::DownloadCompleted)
                                            finishJob(j->id, true, {});
                                        else if (st == QWebEngineDownloadRequest::DownloadCancelled ||
                                                 st == QWebEngineDownloadRequest::DownloadInterrupted)
                                            finishJob(j->id, false, req->interruptReasonString());
                                    }
                                });
                        QDir().mkpath(QFileInfo(it->destPath).absolutePath());
                        req->setDownloadDirectory(
                            QFileInfo(it->destPath).absolutePath());
                        req->setDownloadFileName(
                            QFileInfo(it->destPath).fileName());
                        req->accept();
                        return;
                    }
                }
            });
    m_page = new QWebEnginePage(m_profile, this);
}

void WebEngineDownloadBackend::start(qint64 id, const QString &url,
                                     const QString &destPath) {
    cancel(id); // replace any stale job under this id
    Job job;
    job.id = id;
    job.url = QUrl(url);
    job.destPath = destPath;
    job.timeout = new QTimer(this);
    job.timeout->setSingleShot(true);
    connect(job.timeout, &QTimer::timeout, this, [this, id] {
        cancel(id);
        finishJob(id, false, tr("Browser download timed out"));
    });
    job.timeout->start(120000);
    m_jobs.insert(id, job);
    m_page->download(QUrl(url));
}

void WebEngineDownloadBackend::cancel(qint64 id) {
    auto it = m_jobs.find(id);
    if (it == m_jobs.end())
        return;
    if (it->req)
        it->req->cancel();
    if (it->timeout) {
        it->timeout->stop();
        it->timeout->deleteLater();
    }
    QFile::remove(it->destPath);
    QFile::remove(it->destPath + QStringLiteral(".crdownload"));
    m_jobs.erase(it);
}

bool WebEngineDownloadBackend::fetchBlocking(const QString &url,
                                            const QString &destPath,
                                            QString *error, int timeoutMs) {
    if (QThread::currentThread() != thread()) {
        bool ok = false;
        QString err;
        QMetaObject::invokeMethod(
            this,
            [&ok, &err, &url, &destPath, timeoutMs, this] {
                ok = fetchBlocking(url, destPath, &err, timeoutMs);
            },
            Qt::BlockingQueuedConnection);
        if (error)
            *error = err;
        return ok;
    }
    QEventLoop loop;
    bool ok = false;
    QString err = tr("Download did not start");
    const qint64 id = --m_autoId;
    QMetaObject::Connection doneConn = connect(
        this, &WebEngineDownloadBackend::finished, &loop,
        [&](qint64 fid, bool fok, const QString &ferr) {
            if (fid != id)
                return;
            ok = fok;
            err = ferr;
            loop.quit();
        });
    QTimer timer;
    timer.setSingleShot(true);
    connect(&timer, &QTimer::timeout, &loop, [&] {
        cancel(id);
        ok = false;
        err = tr("Browser download timed out");
        loop.quit();
    });
    start(id, url, destPath);
    timer.start(timeoutMs);
    loop.exec();
    disconnect(doneConn);
    if (error)
        *error = err;
    return ok;
}

WebEngineDownloadBackend::Job *WebEngineDownloadBackend::jobForRequest(
    QWebEngineDownloadRequest *req) {
    for (auto it = m_jobs.begin(); it != m_jobs.end(); ++it) {
        if (it->req == req)
            return &it.value();
    }
    return nullptr;
}

void WebEngineDownloadBackend::finishJob(qint64 id, bool ok,
                                         const QString &error) {
    auto it = m_jobs.find(id);
    if (it == m_jobs.end())
        return;
    Job job = it.value();
    m_jobs.erase(it);
    if (job.timeout) {
        job.timeout->stop();
        job.timeout->deleteLater();
    }
    if (ok) {
        if (!QFile::exists(job.destPath) || QFileInfo(job.destPath).size() == 0)
            ok = false;
        else if (fileLooksLikeHtml(job.destPath))
            ok = false;
    }
    if (!ok) {
        QFile::remove(job.destPath);
        QFile::remove(job.destPath + QStringLiteral(".crdownload"));
    }
    emit finished(id, ok,
                  ok ? QString()
                     : (error.isEmpty()
                            ? tr("Server returned a page, not media "
                                 "(challenge still active — solve once in "
                                 "Capture in browser…)")
                            : error));
}

void WebApiFetcher::setProfilePath(const QString &path) {
    profilePathStore() = path;
}

QWebEngineProfile *WebApiFetcher::captureProfile() {
    return sharedCaptureProfile(profilePathStore());
}

QString &WebApiFetcher::profilePathStore() {
    static QString path;
    return path;
}

WebApiFetcher::Result WebApiFetcher::fetchJsonBlocking(const QUrl &url,
                                                       int timeoutMs) {
    if (QThread::currentThread() != QCoreApplication::instance()->thread()) {
        // Worker thread (e.g. the prefs Test probe): hop to the GUI
        // thread, which runs the same nested-loop fetch below. The GUI
        // thread is free (we never block it), so this cannot deadlock.
        Result out;
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [&] { out = fetchJsonBlocking(url, timeoutMs); },
            Qt::BlockingQueuedConnection);
        return out;
    }
    QEventLoop loop;
    auto *fetcher = new WebApiFetcher(url, timeoutMs);
    fetcher->m_loop = &loop;
    fetcher->start();
    loop.exec();
    Result out = fetcher->m_result;
    fetcher->deleteLater();
    return out;
}

WebApiFetcher::WebApiFetcher(const QUrl &url, int timeoutMs)
    : m_url(url), m_timeoutMs(timeoutMs) {}

void WebApiFetcher::start() {
    // NOTE: caller guarantees the GUI thread (see fetchJsonBlocking).
    m_profile = sharedCaptureProfile(profilePathStore());
    m_view = new QWebEngineView();
    m_view->setVisible(false);
    m_page = new QWebEnginePage(m_profile, m_view);
    m_view->setPage(m_page);
    m_timeout = new QTimer(this);
    m_timeout->setSingleShot(true);
    connect(m_timeout, &QTimer::timeout, this, [this] {
        finish(0, {},
               tr("Browser fetch timed out (challenge unsolved in %1s)")
                   .arg(m_timeoutMs / 1000));
    });
    m_timeout->start(m_timeoutMs);
    m_poll = new QTimer(this);
    connect(m_poll, &QTimer::timeout, this, &WebApiFetcher::poll);
    connect(m_page, &QWebEnginePage::loadFinished, this,
            [this](bool) {
                // Challenge interstitials reload/redirect: poll until
                // JSON lands or the timeout fires.
                if (!m_poll->isActive())
                    m_poll->start(500);
            });
    m_view->load(m_url);
    // First poll shortly after load in case finished already fired.
    QTimer::singleShot(1500, this, [this] {
        if (!m_finished && !m_poll->isActive())
            m_poll->start(500);
    });
}

void WebApiFetcher::poll() {
    if (m_finished)
        return;
    m_page->runJavaScript(
        QStringLiteral("(document.body ? document.body.innerText : '')"
                       ".slice(0, 1048576)"),
        [this](const QVariant &v) {
            if (m_finished)
                return;
            const QString text = v.toString().trimmed();
            if (text.startsWith(u'{') || text.startsWith(u'[')) {
                finish(200, text.toUtf8(), {});
                return;
            }
            // Still a challenge/login page: keep polling. Detect a login
            // redirect early so the wait ends with advice, not silence.
            if (text.contains(QStringLiteral("login"),
                              Qt::CaseInsensitive) &&
                (text.contains(QStringLiteral("password"),
                                Qt::CaseInsensitive) ||
                 text.contains(QStringLiteral("username"),
                                Qt::CaseInsensitive)) &&
                text.size() < 20000)
                finish(0, {}, tr("Browser landed on a login page"));
        });
}

void WebApiFetcher::finish(int http, const QByteArray &body,
                           const QString &error) {
    if (m_finished)
        return;
    m_finished = true;
    if (m_timeout)
        m_timeout->stop();
    if (m_poll)
        m_poll->stop();
    m_result.http = http;
    m_result.body = body;
    m_result.error = error;
    // View-first teardown (same rule as the dialog destructor).
    m_page = nullptr; // parented to the view
    m_view->stop();
    m_view->deleteLater();
    m_view = nullptr;
    if (m_loop)
        m_loop->quit();
}
