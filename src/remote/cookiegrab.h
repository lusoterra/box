#pragma once

// Cookie handling for captcha/Cloudflare-walled boorus.
//
// Some instances sit behind captcha or Cloudflare challenges that plain
// QNAM requests can't pass. The fix is the same one gallery-dl users
// know: bring a browser session's cookies. Two ways to fill them in:
//
// - CookieEditorDialog: paste them by hand (one "name=value" per line;
//   a full "a=1; b=2" header paste or Set-Cookie lines are tolerated).
// - CookieGrabDialog: an embedded browser pointed at the booru. Log in,
//   solve the captcha, then "Use these cookies" harvests the session
//   jar into the entry. Nothing is stored in the profile afterwards
//   beyond the login itself; only the harvested name=value pairs are
//   kept, per booru, in settings.ini next to the API keys.
#include <QDialog>
#include <QEventLoop>
#include <QNetworkCookie>
#include <QStringList>
#include <QTimer>
#include <QUrl>

#include <functional>

class QLabel;
class QPlainTextEdit;
class QPushButton;
class QWebEngineView;
class QWebEnginePage;
class QWebEngineProfile;
class QWebEngineDownloadRequest;

#include "webdlbackend.h"

// WebEngine-backed media downloads for fingerprint-walled hosts (see
// webdlbackend.h). GUI thread only; injected into DownloadManager and
// ThumbLoader by the app. Interactive challenges that need a human click
// ("It is now okay to proceed" / "Verify you are human") fail fast with
// advice instead of stalling to the timeout - one manual solve in the
// capture browser unblocks everything until the next expiry.
class WebApiFetcher : public QObject {
    Q_OBJECT
public:
    struct Result {
        int http = 0;      // 200 when JSON was found, 0 on timeout/failure
        QByteArray body;   // raw page text (JSON on success)
        QString error;     // empty on success
    };
    static Result fetchJsonBlocking(const QUrl &url, int timeoutMs = 30000);
    // Capture profile dir (<dataDir>/webprofile, set by the app): the
    // fallback inherits the solved session. Empty = blank session jar.
    static void setProfilePath(const QString &path);
    // Shared profile instance (GUI thread): API fallback and media
    // downloads ride the same solved session.
    static QWebEngineProfile *captureProfile();

private:
    explicit WebApiFetcher(const QUrl &url, int timeoutMs);
    static QString &profilePathStore();
    void start();
    void poll();
    void finish(int http, const QByteArray &body, const QString &error);

    QUrl m_url;
    int m_timeoutMs;
    QWebEngineProfile *m_profile = nullptr;
    QWebEngineView *m_view = nullptr;
    QWebEnginePage *m_page = nullptr;
    QTimer *m_timeout = nullptr;
    QTimer *m_poll = nullptr;
    QEventLoop *m_loop = nullptr;
    Result m_result;
    bool m_finished = false;
};

// WebEngine-backed media downloads for fingerprint-walled hosts (see
// webdlbackend.h). GUI thread only; injected into DownloadManager and
// ThumbLoader by the app. Interactive challenges that need a human click
// ("It is now okay to proceed" / "Verify you are human") fail fast with
// advice instead of stalling to the timeout - one manual solve in the
// capture browser unblocks everything until the next expiry.
class WebEngineDownloadBackend : public WebDownloadBackend {
    Q_OBJECT
public:
    explicit WebEngineDownloadBackend(QObject *parent = nullptr);

public slots:
    void start(qint64 id, const QString &url,
               const QString &destPath) override;
    void cancel(qint64 id) override;

public:
    bool fetchBlocking(const QString &url, const QString &destPath,
                       QString *error = nullptr,
                       int timeoutMs = 60000) override;

private:
    struct Job {
        qint64 id = -1;
        QUrl url;
        QString destPath;
        QWebEngineDownloadRequest *req = nullptr;
        QTimer *timeout = nullptr;
        qint64 received = 0;
        qint64 total = 0;
    };
    void finishJob(qint64 id, bool ok, const QString &error);
    Job *jobForRequest(QWebEngineDownloadRequest *req);

    QWebEngineProfile *m_profile = nullptr;
    QWebEnginePage *m_page = nullptr;
    QMap<qint64, Job> m_jobs;
    qint64 m_autoId = (1LL << 60); // blocking-fetch ids (manager ids are small)
};

// Manual editor. Construct with the entry's current list; cookies()
// returns the normalized list after accept().
class CookieEditorDialog : public QDialog {
    Q_OBJECT
public:
    explicit CookieEditorDialog(const QStringList &cookies,
                                QWidget *parent = nullptr);
    QStringList cookies() const { return m_cookies; }

private:
    void harvest();
    QPlainTextEdit *m_edit = nullptr;
    QStringList m_cookies;
};

// Browser capture. Needs the site's base URL (login page) and a folder
// for the capture profile (login persistence between captures).
// cookies() returns the normalized, domain-filtered list after accept().
class CookieGrabDialog : public QDialog {
    Q_OBJECT
public:
    CookieGrabDialog(const QString &siteLabel, const QString &baseUrl,
                     const QString &profilePath, QWidget *parent = nullptr,
                     const QString &wallKind = {},
                     const QUrl &startUrl = {});
    // Wall that triggered this capture ("pow" | "captcha" | "challenge"):
    // tailors the in-dialog guidance (PoW self-solves on bundle pages,
    // captchas need a human) because the wall often only fires once the
    // site is actually used, not on the landing page.
    ~CookieGrabDialog() override;
    QStringList cookies() const { return m_cookies; }
    // Silent clearance renewal: loads the page in a hidden view on the
    // persistent capture profile - its JS re-runs proof-of-work and
    // refreshes short-lived cookies with no puzzle to solve - then
    // harvests the jar. done() gets normalized host cookies (empty when
    // the page is still challenged). GUI thread only.
    static void refreshSilent(const QString &baseUrl,
                              const QString &profilePath,
                              std::function<void(const QStringList &)> done);
    // Idle-time primer: first WebEngine use initializes Chromium on the
    // GUI thread and can stall/repaint every window for a beat. Run once
    // after startup so opening a capture dialog later doesn't blink the
    // app. Safe to call repeatedly (second calls are no-ops in effect).
    static void prime(const QString &profilePath);

protected:
    void showEvent(QShowEvent *e) override; // deferred first load: the
        // dialog chrome paints before the renderer spins up

private slots:

private slots:
    void onLoadFinished(bool ok);
    void onUrlChanged(const QUrl &url);
    void onCookieAdded(const QNetworkCookie &cookie);
    void useCookies();
    void autoHarvest(); // solved-challenge collection that stays open
    void finishHarvest();

private:
    void setStatus(const QString &text);
    QString m_host;
    QUrl m_first;      // loaded on first show, not in the ctor
    bool m_loaded = false;
    QWebEngineView *m_view = nullptr;
    QWebEnginePage *m_page = nullptr;
    QWebEngineProfile *m_profile = nullptr;
    QLabel *m_status = nullptr;
    QLabel *m_count = nullptr;
    QList<QNetworkCookie> m_seen; // session jar accumulation
    QStringList m_cookies;
    QPushButton *m_useBtn = nullptr;
    bool m_flushing = false; // harvest round-trip in flight
    bool m_autoAccept = true; // manual harvest closes the dialog
    bool m_armed = false;    // startup cookie burst is over
    bool m_autoFired = false; // challenge-solved auto-collect already triggered
    bool m_cfSeen = false;   // a Cloudflare cookie appeared this session
    bool m_restoredClearance = false; // cf_clearance predates this capture
};
