#include "postmeta.h"

#include "powsolver.h"
#include "remotesource.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QRegularExpression>
#include <QTextDocumentFragment>
#include <QThreadPool>
#include <QUrl>
#include <QUrlQuery>
#include <QXmlStreamAttributes>
#include <QXmlStreamReader>

PostMetaFetcher::PostMetaFetcher(QObject *parent)
    : QObject(parent), m_nam(new QNetworkAccessManager(this)) {}

void PostMetaFetcher::setCreds(const QString &danUser, const QString &danKey,
                               const QString &gelUser, const QString &gelKey) {
    m_danUser = danUser;
    m_danKey = danKey;
    m_gelUser = gelUser;
    m_gelKey = gelKey;
}

namespace {
// withCookies=false for Cloudflare/Turnstile hosts: their clearance is
// bound to the capture browser's fingerprint, so presenting it from QNAM
// can *cause* the very challenge we are trying to avoid (the meta
// endpoints answer fine anonymously - live danbooru.donmai.us).
QNetworkRequest timedRequest(const QUrl &url, bool withCookies = true) {
    QNetworkRequest req(url);
    req.setTransferTimeout(15000);
    if (withCookies)
        RemoteSource::applyCookies(req); // clearance header, if any
    req.setHeader(QNetworkRequest::UserAgentHeader,
                  QStringLiteral("box/0.1 (post meta)"));
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    return req;
}

void applyBasicAuth(QNetworkRequest &req, const QString &user, const QString &secret) {
    if (user.trimmed().isEmpty() || secret.isEmpty())
        return;
    req.setRawHeader("Authorization",
                     "Basic " + (user.trimmed() + u':' + secret).toUtf8().toBase64());
}
} // namespace

void PostMetaFetcher::fetch(const QString &siteKey, const QString &baseUrl,
                            qint64 postId, BooruEngine engine,
                            bool hasNotesHint, int parts) {
    cancel();
    m_site = siteKey;
    m_baseUrl = baseUrl;
    m_engine = engine;
    m_postId = postId;
    m_powRetried = false;
    m_notes.clear();
    m_comments.clear();
    m_commentary.clear();
    m_notesDone = false;
    m_commentsDone = false;
    m_commentaryDone = false;
    m_notesOk = true;
    m_commentsOk = true;
    m_commentaryOk = true;
    m_browserRetried.clear();
    m_notesReq = m_commentsReq = m_commentaryReq = false;
    m_notesUrl =
        PostMeta::notesUrl(engine, baseUrl, postId, m_gelUser, m_gelKey);
    Q_UNUSED(hasNotesHint); // notesUrl() already picks the reliable shape
    m_commentsUrl =
        PostMeta::commentsUrl(engine, baseUrl, postId, m_gelUser, m_gelKey);
    m_commentaryUrl = PostMeta::commentaryUrl(engine, baseUrl, postId);
    // Legs the caller did not ask for are dropped here, so the rest of
    // the pipeline (incl. browser fallbacks) sees them as "done, empty".
    if (!(parts & NotesPart))
        m_notesUrl.clear();
    if (!(parts & CommentsPart))
        m_commentsUrl.clear();
    if (!(parts & CommentaryPart))
        m_commentaryUrl.clear();
    const RemotePosts::SiteQuirk *hostQuirk =
        RemotePosts::quirkForHost(QUrl(baseUrl).host().toLower());
    // Turnstile/Cloudflare hosts: go anonymous (see timedRequest).
    const bool withCookies = !(hostQuirk && hostQuirk->browserApi);
    QNetworkRequest notesReq = timedRequest(m_notesUrl, withCookies);
    QNetworkRequest commentsReq = timedRequest(m_commentsUrl, withCookies);
    QNetworkRequest commentaryReq = timedRequest(m_commentaryUrl, withCookies);
    // Unconditional: clears a stale proxy when the entry drops it.
    m_nam->setProxy(RemotePosts::proxyFromSpec(
        RemotePosts::proxyForUrl(baseUrl)));
    if (engine == BooruEngine::Szurubooru) {
        // Szurubooru's API answers 406 without an explicit Accept header.
        notesReq.setRawHeader("Accept", "application/json");
        commentsReq.setRawHeader("Accept", "application/json");
        commentaryReq.setRawHeader("Accept", "application/json");
    } else if (RemotePosts::isPhilomenaFamily(engine) ||
               engine == BooruEngine::Hybooru) {
        // Philomena/HyBooru details: ask for the JSON representation only.
        notesReq.setRawHeader("Accept", "application/json");
        commentaryReq.setRawHeader("Accept", "application/json");
    }
    if (notesReq.url().isEmpty())
        m_notesDone = true; // engine has no notes API
    if (commentsReq.url().isEmpty())
        m_commentsDone = true;
    if (commentaryReq.url().isEmpty())
        m_commentaryDone = true;
    if (engine == BooruEngine::Danbooru) {
        applyBasicAuth(notesReq, m_danUser, m_danKey);
        applyBasicAuth(commentsReq, m_danUser, m_danKey);
        applyBasicAuth(commentaryReq, m_danUser, m_danKey);
    } else if (engine == BooruEngine::Gelbooru ||
               engine == BooruEngine::Safebooru) {
        // Gelbooru dapi takes creds as query params (already in the URL).
    }
    // Plain QNAM first, even on `browserApi` hosts: the meta endpoints
    // (/notes.json, /comments.json) answer fine anonymously where the
    // search API is challenged (live danbooru.donmai.us 2026-11-21).
    // Forcing the hidden browser here made every notes fetch fail with
    // "Cloudflare wants a click" on posts whose search row loaded fine.
    // Each reply handler falls back to the capture profile once when the
    // plain attempt is walled (retryPartInBrowser).
    if (!notesReq.url().isEmpty()) {
        m_notesReq = true;
        m_notesOk = false; // issued: must prove success below
        m_notesErr.clear();
        m_notesReply = m_nam->get(notesReq);
        connect(m_notesReply, &QNetworkReply::finished, this, [this] {
            QNetworkReply *r = m_notesReply;
            m_notesReply = nullptr;
            // HTTP-level failure or an unparseable body (challenge page,
            // wall HTML): not a definitive empty - the caller retries.
            // Bodies read unconditionally: a 449 surfaces as a reply
            // error but still carries the pow/captcha verdict below.
            bool ok = false;
            int http = 0;
            QByteArray body;
            if (r) {
                http =
                    r->attribute(QNetworkRequest::HttpStatusCodeAttribute)
                        .toInt();
                body = r->readAll();
                if (r->error() == QNetworkReply::NoError && http / 100 == 2) {
                    m_notes = PostMeta::parseNotes(body, &ok, m_postId);
                    if (!ok)
                        m_notesErr = tr("Unparseable response");
                } else if (r->error() != QNetworkReply::NoError)
                    m_notesErr = r->errorString();
                else
                    m_notesErr = tr("HTTP %1").arg(http);
                r->deleteLater();
            }
            m_notesOk = ok;
            if (!ok && maybePowRetry(body, http))
                return; // native re-solve in flight; results land as new done
            if (!ok && retryPartInBrowser(0))
                return; // walled: capture-profile retry owns this part
            m_notesDone = true;
            finishPart();
        });
    }
    if (!commentsReq.url().isEmpty()) {
        m_commentsReq = true;
        m_commentsOk = false; // issued: must prove success below
        m_commentsErr.clear();
        m_commentsReply = m_nam->get(commentsReq);
        connect(m_commentsReply, &QNetworkReply::finished, this, [this] {
            QNetworkReply *r = m_commentsReply;
            m_commentsReply = nullptr;
            bool ok = false;
            int http = 0;
            QByteArray body;
            if (r) {
                http =
                    r->attribute(QNetworkRequest::HttpStatusCodeAttribute)
                        .toInt();
                body = r->readAll();
                if (http == 404) {
                    // No comments endpoint / none posted: treat as empty,
                    // not a failure (avoids misleading "failed to load").
                    m_comments.clear();
                    ok = true;
                } else if (r->error() == QNetworkReply::NoError && http / 100 == 2) {
                    m_comments = PostMeta::parseComments(body, &ok, m_postId);
                    if (!ok)
                        m_commentsErr = tr("Unparseable response");
                } else if (r->error() != QNetworkReply::NoError)
                    m_commentsErr = r->errorString();
                else
                    m_commentsErr = tr("HTTP %1").arg(http);
                r->deleteLater();
            }
            m_commentsOk = ok;
            if (!ok && maybePowRetry(body, http))
                return; // native re-solve in flight; results land as new done
            if (!ok && retryPartInBrowser(1))
                return; // walled: capture-profile retry owns this part
            m_commentsDone = true;
            finishPart();
        });
    }
    if (!commentaryReq.url().isEmpty()) {
        m_commentaryReq = true;
        m_commentaryOk = false; // issued: must prove success below
        m_commentaryErr.clear();
        m_commentaryReply = m_nam->get(commentaryReq);
        connect(m_commentaryReply, &QNetworkReply::finished, this, [this] {
            QNetworkReply *r = m_commentaryReply;
            m_commentaryReply = nullptr;
            bool ok = false;
            int http = 0;
            QByteArray body;
            if (r) {
                http =
                    r->attribute(QNetworkRequest::HttpStatusCodeAttribute)
                        .toInt();
                body = r->readAll();
                if (http == 404) {
                    // Danbooru: no artist commentary row for this post
                    // (the collection endpoint 404s instead of returning
                    // an empty array). Definitive empty, not a failure.
                    m_commentary.clear();
                    ok = true;
                } else if (r->error() == QNetworkReply::NoError &&
                           http / 100 == 2) {
                    m_commentary =
                        PostMeta::parseCommentary(m_engine, body, &ok);
                    if (!ok)
                        m_commentaryErr = tr("Unparseable response");
                } else if (r->error() != QNetworkReply::NoError)
                    m_commentaryErr = r->errorString();
                else
                    m_commentaryErr = tr("HTTP %1").arg(http);
                r->deleteLater();
            }
            m_commentaryOk = ok;
            if (!ok && maybePowRetry(body, http))
                return;
            if (!ok && retryPartInBrowser(2))
                return; // walled: capture-profile retry owns this part
            m_commentaryDone = true;
            finishPart();
        });
    }
    finishPart(); // in case every URL was empty
}

void PostMetaFetcher::cancel() {
    ++m_gen; // drop results from browser workers started before this call
    for (QNetworkReply **slot :
         {&m_notesReply, &m_commentsReply, &m_commentaryReply}) {
        if (*slot) {
            disconnect(*slot, nullptr, this, nullptr);
            (*slot)->abort();
            (*slot)->deleteLater();
            *slot = nullptr;
        }
    }
    m_notesDone = true;
    m_commentsDone = true;
    m_commentaryDone = true;
}

void PostMetaFetcher::startBrowserPart(int part, const QUrl &url) {
    const auto web = RemoteSource::webFetcher();
    if (!web || !url.isValid()) {
        // Never leave a part "in flight" forever: report the failure so
        // finishPart can emit done.
        applyBrowserPart(m_gen, part, 0, {},
                         tr("Browser fetch unavailable"));
        return;
    }
    const int gen = m_gen;
    QPointer<PostMetaFetcher> self(this);
    QThreadPool::globalInstance()->start([web, url, gen, part, self] {
        const RemoteSource::WebFetchResult r = web(url, 20000);
        if (!self)
            return;
        QMetaObject::invokeMethod(
            self.data(),
            [self, gen, part, r] {
                if (self)
                    self->applyBrowserPart(gen, part, r.http, r.body, r.error);
            },
            Qt::QueuedConnection);
    });
}

void PostMetaFetcher::applyBrowserPart(int gen, int part, int http,
                                       const QByteArray &body,
                                       const QString &error) {
    if (gen != m_gen)
        return; // a newer fetch owns the UI
    bool ok = false;
    QString err = error;
    if (http / 100 == 2 && !body.isEmpty()) {
        if (part == 0)
            m_notes = PostMeta::parseNotes(body, &ok, m_postId);
        else if (part == 1)
            m_comments = PostMeta::parseComments(body, &ok, m_postId);
        else
            m_commentary = PostMeta::parseCommentary(m_engine, body, &ok);
        if (!ok && err.isEmpty())
            err = tr("Unparseable response");
    } else if (err.isEmpty()) {
        err = http > 0 ? tr("HTTP %1").arg(http) : tr("Browser fetch failed");
    }
    switch (part) {
    case 0:
        m_notesOk = ok;
        m_notesErr = ok ? QString() : err;
        m_notesDone = true;
        break;
    case 1:
        m_commentsOk = ok;
        m_commentsErr = ok ? QString() : err;
        m_commentsDone = true;
        break;
    default:
        m_commentaryOk = ok;
        m_commentaryErr = ok ? QString() : err;
        m_commentaryDone = true;
        break;
    }
    finishPart();
}

bool PostMetaFetcher::retryPartInBrowser(int part) {
    if (m_browserRetried.contains(part))
        return false; // one fallback per part per fetch
    const QUrl url = part == 0 ? m_notesUrl
                               : part == 1 ? m_commentsUrl : m_commentaryUrl;
    if (url.isEmpty() || !RemoteSource::webFetcher())
        return false; // no browser stack: the plain failure stands
    m_browserRetried.insert(part);
    switch (part) {
    case 0:
        m_notesOk = false;
        m_notesErr.clear();
        break;
    case 1:
        m_commentsOk = false;
        m_commentsErr.clear();
        break;
    default:
        m_commentaryOk = false;
        m_commentaryErr.clear();
        break;
    }
    startBrowserPart(part, url);
    return true;
}

void PostMetaFetcher::finishPart() {
    if (!m_notesDone || !m_commentsDone || !m_commentaryDone)
        return;
    QString err = m_notesErr;
    if (err.isEmpty())
        err = m_commentsErr;
    // Commentary is optional enrichment: a failure there must not hide
    // notes/comments that loaded fine (nor paint an error banner on
    // commentary-only engines).
    // Parts are independent: a failed comments leg must never discard
    // successfully parsed notes (that showed every Gelbooru post as
    // "post details failed", with the annotations thrown away).
    bool anyOk = false;
    bool anyReq = false;
    if (m_notesReq) {
        anyReq = true;
        anyOk = anyOk || m_notesOk;
    }
    if (m_commentsReq) {
        anyReq = true;
        anyOk = anyOk || m_commentsOk;
    }
    if (m_commentaryReq) {
        anyReq = true;
        anyOk = anyOk || m_commentaryOk;
    }
    emit done(m_site, m_postId, m_notes, m_comments, m_commentary,
              anyReq ? anyOk : true, err);
}

// Native PoW re-solve for 449-gated stacks (Nuuru): the token is
// IP-bound, so it must be minted on this exact NAM (same proxy and
// egress, same jar) before retrying once. Captchas stay human -
// the failure message says to solve in the browser, then Retry.
bool PostMetaFetcher::maybePowRetry(const QByteArray &body, int http) {
    if (m_powRetried || m_engine != BooruEngine::Nuuru || body.isEmpty())
        return false;
    const QString text = QString::fromUtf8(body);
    if (!text.contains(QStringLiteral("pow_required"), Qt::CaseInsensitive)) {
        if (text.contains(QStringLiteral("captcha_required"),
                          Qt::CaseInsensitive)) {
            const QString msg =
                tr("Captcha required - solve it in the capture browser, "
                   "then Retry.");
            if (m_notesErr.isEmpty() && m_commentsErr.isEmpty()) {
                m_notesErr = msg;
                m_commentsErr = msg;
            }
        }
        return false;
    }
    Q_UNUSED(http);
    m_powRetried = true; // set before the async solve: both parts funnel here
    PowSolver::solve(m_nam, m_baseUrl, this,
                     [this](bool ok, const QStringList &, const QString &) {
                         if (!ok)
                             return; // solve failed: original failure stands
                         fetch(m_site, m_baseUrl, m_postId, m_engine);
                         m_powRetried = true; // fetch() resets; keep the guard
                     });
    return true;
}

namespace PostMeta {

QUrl notesUrl(BooruEngine engine, const QString &baseUrl, qint64 postId,
              const QString &gelUser, const QString &gelKey) {
    const QString id = QString::number(postId);
    if (engine == BooruEngine::Szurubooru) {
        // Annotations ride the post detail object (`notes[]` polygons);
        // there is no separate notes endpoint. Without this URL the notes
        // part was marked done-empty, so posts flagged `notes` never
        // rendered their boxes (live nekomaru.moe 2026-11-22).
        return QUrl(baseUrl + QStringLiteral("/api/post/") + id);
    }
    if (engine == BooruEngine::Shimmie ||
        engine == BooruEngine::Nuuru ||
        RemotePosts::isPhilomenaFamily(engine))
        return {}; // no notes endpoint on these engines
    if (engine == BooruEngine::Hybooru) {
        // Hydrus notes ride the post detail object: rect notes are the
        // image annotations, rect-null rows are commentary.
        return QUrl(baseUrl + QStringLiteral("/api/post/") + id);
    }
    if (engine == BooruEngine::Danbooru || engine == BooruEngine::E621) {
        // e621 is a Danbooru fork: same notes shape (verified live).
        QUrl url(baseUrl + QStringLiteral("/notes.json"));
        QUrlQuery q;
        q.addQueryItem(QStringLiteral("search[post_id]"), id);
        q.addQueryItem(QStringLiteral("limit"), QStringLiteral("100"));
        url.setQuery(q);
        return url;
    }
    if (engine == BooruEngine::Gelbooru || engine == BooruEngine::Safebooru) {
        // dapi `s=note` answers HTTP 401 with an empty body anonymously
        // (live 2026-11-21), and an empty table even with creds for posts
        // whose page carries annotations. The public post page always
        // embeds them (<article data-x…> / note-box divs): use it.
        Q_UNUSED(gelUser);
        Q_UNUSED(gelKey);
        return QUrl(baseUrl + QStringLiteral("/index.php?page=post&s=view&id=") +
                    id);
    }
    // Moebooru family. AnimePictures exposes no note API at all: it
    // must not fall through to Moebooru's URL (a 404 would fail the
    // whole meta fetch for posts that simply have no notes).
    if (engine == BooruEngine::AnimePictures)
        return {};
    return QUrl(baseUrl + QStringLiteral("/note.json?post_id=") + id);
}

QUrl commentsUrl(BooruEngine engine, const QString &baseUrl, qint64 postId,
                 const QString &gelUser, const QString &gelKey) {
    const QString id = QString::number(postId);
    if (engine == BooruEngine::Szurubooru) {
        // The endpoint ignores a postId parameter and returns the whole
        // feed; scope through the query language instead (live-verified).
        QUrl url(baseUrl + QStringLiteral("/api/comments/"));
        QUrlQuery q;
        q.addQueryItem(QStringLiteral("query"),
                       QStringLiteral("post:") + id);
        q.addQueryItem(QStringLiteral("limit"), QStringLiteral("100"));
        url.setQuery(q);
        return url;
    }
    if (engine == BooruEngine::Philomena) {
        // Philomena v1: unscoped comments search, scoped by image_id.
        QUrl url(baseUrl + QStringLiteral("/api/v1/json/search/comments"));
        QUrlQuery q;
        q.addQueryItem(QStringLiteral("q"),
                       QStringLiteral("image_id:") + id);
        q.addQueryItem(QStringLiteral("per_page"), QStringLiteral("100"));
        url.setQuery(q);
        return url;
    }
    if (engine == BooruEngine::BooruOnRails) {
        // v3 exposes the thread on the post itself.
        return QUrl(baseUrl + QStringLiteral("/api/v3/posts/") + id +
                    QStringLiteral("/comments"));
    }
    if (engine == BooruEngine::Shimmie || engine == BooruEngine::Hybooru)
        return {}; // no comment API on these engines (v1 / unknown shape)
    if (engine == BooruEngine::Nuuru) {
        QUrl url(baseUrl + QStringLiteral("/api/booru/posts/") + id +
                 QStringLiteral("/comments"));
        QUrlQuery q;
        q.addQueryItem(QStringLiteral("page"), QStringLiteral("1"));
        q.addQueryItem(QStringLiteral("pageSize"), QStringLiteral("50"));
        url.setQuery(q);
        return url;
    }
    if (engine == BooruEngine::Danbooru || engine == BooruEngine::E621) {
        // e621 is a Danbooru fork: same comments shape (verified live).
        QUrl url(baseUrl + QStringLiteral("/comments.json"));
        QUrlQuery q;
        q.addQueryItem(QStringLiteral("search[post_id]"), id);
        q.addQueryItem(QStringLiteral("only"),
                       QStringLiteral("id,body,creator_name,created_at"));
        q.addQueryItem(QStringLiteral("limit"), QStringLiteral("100"));
        url.setQuery(q);
        return url;
    }
    if (engine == BooruEngine::Gelbooru || engine == BooruEngine::Safebooru) {
        QUrl url(RemotePosts::apiBaseUrl(engine, baseUrl) +
                 QStringLiteral("/index.php"));
        QUrlQuery q;
        q.addQueryItem(QStringLiteral("page"), QStringLiteral("dapi"));
        q.addQueryItem(QStringLiteral("s"), QStringLiteral("comment"));
        q.addQueryItem(QStringLiteral("q"), QStringLiteral("index"));
        q.addQueryItem(QStringLiteral("post_id"), id);
        q.addQueryItem(QStringLiteral("json"), QStringLiteral("1"));
        if (!gelUser.isEmpty() && !gelKey.isEmpty()) {
            q.addQueryItem(QStringLiteral("api_key"), gelKey);
            q.addQueryItem(QStringLiteral("user_id"), gelUser);
        }
        url.setQuery(q);
        return url;
    }
    if (engine == BooruEngine::AnimePictures)
        return {}; // no comment API either (same 404-poisons-meta reason)
    return QUrl(baseUrl + QStringLiteral("/comment.json?post_id=") + id);
}

QUrl commentaryUrl(BooruEngine engine, const QString &baseUrl, qint64 postId) {
    const QString id = QString::number(postId);
    if (engine == BooruEngine::Danbooru) {
        // Plural: /artist_commentary.json answers 404 for every post
        // (live danbooru.donmai.us 2026-11-22), the collection endpoint
        // returns the row array.
        QUrl url(baseUrl + QStringLiteral("/artist_commentaries.json"));
        QUrlQuery q;
        q.addQueryItem(QStringLiteral("search[post_id]"), id);
        q.addQueryItem(QStringLiteral("limit"), QStringLiteral("1"));
        url.setQuery(q);
        return url;
    }
    // Philomena family (v1 images / v3 posts) and HyBooru expose the
    // commentary-like text on the single-post object itself.
    if (RemotePosts::isPhilomenaFamily(engine))
        return QUrl(baseUrl + RemotePosts::philomenaItemPath(engine) + id);
    if (engine == BooruEngine::Hybooru)
        return QUrl(baseUrl + QStringLiteral("/api/post/") + id);
    return {};
}

namespace {
// HTML attribute values arrive entity-escaped ("&quot;", "&#039;", "&amp;").
// Rich-text parsing decodes them and strips any accidental inline tags.
QString decodeHtmlText(const QString &in) {
    if (in.isEmpty())
        return {};
    return QTextDocumentFragment::fromHtml(in).toPlainText().trimmed();
}

int stylePx(const QString &style, const QString &name) {
    const QRegularExpression re(
        QStringLiteral("(?:^|;)\\s*%1\\s*:\\s*(-?[\\d.]+)px").arg(name),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch m = re.match(style);
    return m.hasMatch() ? qRound(m.captured(1).toDouble()) : 0;
}

QString styleAttr(const QString &attrs) {
    const QRegularExpression re(QStringLiteral("style\\s*=\\s*\"([^\"]*)\""),
                                QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch m = re.match(attrs);
    return m.hasMatch() ? m.captured(1) : QString();
}

// Gelbooru/Safebooru post-page annotations. Public pages embed them in
// two shapes: gelbooru.com uses <article data-x data-y data-width
// data-height data-body="...">; safebooru.org renders note-box/note-body
// divs with inline geometry and the text as element content. The dapi
// s=note table is unreliable (empty on gelbooru.com), so the viewer
// reads the page when the post says it has notes.
QVector<PostNote> parseGelbooruNotesHtml(const QByteArray &body, bool *ok) {
    if (ok)
        *ok = false;
    const QString html = QString::fromUtf8(body);
    const bool postPage =
        html.contains(QStringLiteral("note-container")) ||
        html.contains(QStringLiteral("id=\"notes\""));
    QVector<PostNote> out;

    static const QRegularExpression articleRe(
        QStringLiteral("<article\\b([^>]*)>"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression attrRe(
        QStringLiteral("data-(x|y|width|height|body)\\s*=\\s*\"([^\"]*)\""),
        QRegularExpression::CaseInsensitiveOption);
    auto articles = articleRe.globalMatch(html);
    while (articles.hasNext()) {
        const QRegularExpressionMatch m = articles.next();
        QHash<QString, QString> attrs;
        auto it = attrRe.globalMatch(m.captured(1));
        while (it.hasNext()) {
            const QRegularExpressionMatch am = it.next();
            attrs.insert(am.captured(1).toLower(), am.captured(2));
        }
        if (!attrs.contains(QStringLiteral("body")))
            continue;
        PostNote n;
        n.x = attrs.value(QStringLiteral("x")).toInt();
        n.y = attrs.value(QStringLiteral("y")).toInt();
        n.w = attrs.value(QStringLiteral("width")).toInt();
        n.h = attrs.value(QStringLiteral("height")).toInt();
        n.body = decodeHtmlText(attrs.value(QStringLiteral("body")));
        if (!n.body.isEmpty() && n.w > 0 && n.h > 0)
            out.append(n);
    }
    if (!out.isEmpty()) {
        if (ok)
            *ok = true;
        return out;
    }

    // safebooru.org shape: note-box-<id> style + note-body-<id> text.
    QHash<QString, QRect> boxes;
    static const QRegularExpression boxRe(
        QStringLiteral("<div\\b([^>]*id=\"note-box-(\\d+)\"[^>]*)>"),
        QRegularExpression::CaseInsensitiveOption);
    auto boxIt = boxRe.globalMatch(html);
    while (boxIt.hasNext()) {
        const QRegularExpressionMatch m = boxIt.next();
        const QString attrs = m.captured(1);
        const QString style = styleAttr(attrs);
        const QRect r(stylePx(style, QStringLiteral("left")),
                      stylePx(style, QStringLiteral("top")),
                      stylePx(style, QStringLiteral("width")),
                      stylePx(style, QStringLiteral("height")));
        if (r.width() > 0 && r.height() > 0)
            boxes.insert(m.captured(2), r);
    }
    static const QRegularExpression noteBodyRe(
        QStringLiteral(
            "<div\\b([^>]*id=\"note-body-(\\d+)\"[^>]*)>(.*?)</div>"),
        QRegularExpression::CaseInsensitiveOption |
            QRegularExpression::DotMatchesEverythingOption);
    auto bodyIt = noteBodyRe.globalMatch(html);
    while (bodyIt.hasNext()) {
        const QRegularExpressionMatch m = bodyIt.next();
        const auto box = boxes.constFind(m.captured(2));
        if (box == boxes.constEnd())
            continue;
        PostNote n;
        n.x = box->x();
        n.y = box->y();
        n.w = box->width();
        n.h = box->height();
        n.body = decodeHtmlText(m.captured(3));
        if (!n.body.isEmpty())
            out.append(n);
    }
    if (ok)
        *ok = postPage;
    return out;
}
} // namespace

QVector<PostNote> parseNotes(const QByteArray &body, bool *ok, qint64 postId) {
    QVector<PostNote> out;
    if (ok)
        *ok = false;
    // Gelbooru/Safebooru post-page annotations (see parser above).
    const QByteArray trimmed = body.trimmed();
    if (trimmed.contains("note-container") || trimmed.contains("<article") ||
        trimmed.contains("id=\"notes\""))
        return parseGelbooruNotesHtml(trimmed, ok);
    // Gelbooru's dapi serves the note table as XML even when json=1 is
    // requested (live safebooru.org, 2026-11-15), so a leading '<' routes
    // to the XML reader instead of failing every note fetch there.
    if (trimmed.startsWith('<')) {
        QXmlStreamReader xml(trimmed);
        bool shaped = false; // root must be a notes table, not wall HTML
        while (!xml.atEnd()) {
            xml.readNext();
            if (!xml.isStartElement())
                continue;
            if (xml.name() == u"notes" || xml.name() == u"note")
                shaped = true;
            if (xml.name() != u"note")
                continue;
            const QXmlStreamAttributes a = xml.attributes();
            const auto owner = a.value(u"post_id");
            if (postId > 0 && !owner.isEmpty() &&
                owner.toLongLong() != postId)
                continue; // site-wide dump: not this post's note
            PostNote n;
            n.x = int(a.value(u"x").toInt());
            n.y = int(a.value(u"y").toInt());
            n.w = int(a.value(u"width").toInt());
            n.h = int(a.value(u"height").toInt());
            n.body = a.value(u"body").toString().trimmed();
            if (!n.body.isEmpty())
                out.append(n);
        }
        if (ok)
            *ok = shaped && !xml.hasError();
        return out;
    }
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &err);
    if (err.error != QJsonParseError::NoError)
        return out; // challenge HTML, wall pages: not definitive
    // HyBooru detail object: Hydrus notes carry percentage rects (left,
    // top, width, height are 0..100 of the original image). rect=null
    // rows are commentary and stay in parseCommentary.
    if (doc.isObject()) {
        const QJsonObject root = doc.object();
        const QJsonValue notesVal = root.value(QStringLiteral("notes"));
        // Szurubooru post detail: notes[].polygon is a list of [x,y]
        // corner points, either fractions of the canvas (modern) or raw
        // pixels (old instances). The viewer draws axis-aligned rects, so
        // the polygon's bounding box is the overlay.
        const QJsonArray notesArr = notesVal.toArray();
        if (!notesArr.isEmpty() &&
            notesArr.first().toObject().contains(QStringLiteral("polygon"))) {
            double imageW = root.value(QStringLiteral("canvasWidth")).toDouble();
            if (imageW <= 0)
                imageW = root.value(QStringLiteral("width")).toDouble();
            double imageH =
                root.value(QStringLiteral("canvasHeight")).toDouble();
            if (imageH <= 0)
                imageH = root.value(QStringLiteral("height")).toDouble();
            for (const QJsonValue &v : notesArr) {
                if (!v.isObject())
                    continue;
                const QJsonObject o = v.toObject();
                const QJsonArray poly =
                    o.value(QStringLiteral("polygon")).toArray();
                if (poly.size() < 3)
                    continue;
                double minX = 1e18, minY = 1e18, maxX = -1e18, maxY = -1e18;
                bool normalized = true;
                for (const QJsonValue &pv : poly) {
                    const QJsonArray pt = pv.toArray();
                    if (pt.size() < 2)
                        continue;
                    const double x = pt.at(0).toDouble();
                    const double y = pt.at(1).toDouble();
                    if (x > 1.0 || y > 1.0)
                        normalized = false;
                    minX = qMin(minX, x);
                    minY = qMin(minY, y);
                    maxX = qMax(maxX, x);
                    maxY = qMax(maxY, y);
                }
                if (maxX < minX || maxY < minY)
                    continue;
                if (normalized) {
                    if (imageW <= 0 || imageH <= 0)
                        continue; // no canvas: cannot place the box
                    minX *= imageW;
                    maxX *= imageW;
                    minY *= imageH;
                    maxY *= imageH;
                }
                PostNote n;
                n.x = qRound(minX);
                n.y = qRound(minY);
                n.w = qRound(maxX - minX);
                n.h = qRound(maxY - minY);
                n.body = o.value(QStringLiteral("text")).toString().trimmed();
                if (!n.body.isEmpty() && n.w > 0 && n.h > 0)
                    out.append(n);
            }
            if (ok)
                *ok = true;
            return out;
        }
        const double imageW = root.value(QStringLiteral("width")).toDouble();
        const double imageH = root.value(QStringLiteral("height")).toDouble();
        if (notesVal.isArray() && imageW > 0 && imageH > 0) {
            for (const QJsonValue &v : notesVal.toArray()) {
                if (!v.isObject())
                    continue;
                const QJsonObject o = v.toObject();
                const QJsonValue rect = o.value(QStringLiteral("rect"));
                if (!rect.isObject())
                    continue; // null rect = commentary, not an overlay
                const QJsonObject r = rect.toObject();
                PostNote n;
                n.x = qRound(r.value(QStringLiteral("left")).toDouble() /
                             100.0 * imageW);
                n.y = qRound(r.value(QStringLiteral("top")).toDouble() /
                             100.0 * imageH);
                n.w = qRound(r.value(QStringLiteral("width")).toDouble() /
                             100.0 * imageW);
                n.h = qRound(r.value(QStringLiteral("height")).toDouble() /
                             100.0 * imageH);
                n.body = o.value(QStringLiteral("note")).toString().trimmed();
                if (!n.body.isEmpty() && n.w > 0 && n.h > 0)
                    out.append(n);
            }
            if (ok)
                *ok = true;
            return out;
        }
    }
    if (ok)
        *ok = true;
    QJsonArray arr;
    if (doc.isArray())
        arr = doc.array();
    else if (doc.isObject())
        arr = doc.object().value(QStringLiteral("note")).toArray();
    for (const QJsonValue &v : arr) {
        if (!v.isObject())
            continue;
        const QJsonObject o = v.toObject();
        // Deleted/hidden annotations are not what the site shows.
        const QJsonValue active = o.value(QStringLiteral("is_active"));
        if (active.isBool() && !active.toBool())
            continue;
        if (postId > 0) {
            const qint64 owner =
                o.value(QStringLiteral("post_id")).toVariant().toLongLong();
            if (owner > 0 && owner != postId)
                continue; // unscoped endpoint: another post's annotation
        }
        PostNote n;
        n.x = o.value(QStringLiteral("x")).toInt();
        n.y = o.value(QStringLiteral("y")).toInt();
        n.w = o.value(QStringLiteral("width")).toInt();
        n.h = o.value(QStringLiteral("height")).toInt();
        n.body = o.value(QStringLiteral("body")).toString().trimmed();
        if (!n.body.isEmpty())
            out.append(n);
    }
    return out;
}

QVector<PostComment> parseComments(const QByteArray &body, bool *ok,
                                   qint64 postId) {
    QVector<PostComment> out;
    if (ok)
        *ok = false;
    // Gelbooru dapi XML shape for the comment table (see parseNotes).
    const QByteArray trimmed = body.trimmed();
    if (trimmed.startsWith('<')) {
        QXmlStreamReader xml(trimmed);
        bool shaped = false;
        while (!xml.atEnd()) {
            xml.readNext();
            if (!xml.isStartElement())
                continue;
            if (xml.name() == u"comments" || xml.name() == u"comment")
                shaped = true;
            if (xml.name() != u"comment")
                continue;
            const QXmlStreamAttributes a = xml.attributes();
            if (postId > 0) {
                const auto owner = a.value(u"post_id");
                if (!owner.isEmpty() && owner.toLongLong() != postId)
                    continue;
            }
            PostComment c;
            c.author = a.value(u"creator").toString();
            c.body = a.value(u"body").toString().trimmed();
            c.date = a.value(u"created_at").toString();
            if (!c.body.isEmpty())
                out.append(c);
        }
        if (ok)
            *ok = shaped && !xml.hasError();
        return out;
    }
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &err);
    if (err.error != QJsonParseError::NoError)
        return out; // challenge HTML, wall pages: not definitive
    if (ok)
        *ok = true;
    QJsonArray arr;
    if (doc.isArray())
        arr = doc.array();
    else if (doc.isObject()) {
        // Gelbooru {"comment": [...]}, Nuuru {"comments": [...]},
        // Szurubooru {"results": [...]}.
        arr = doc.object().value(QStringLiteral("comment")).toArray();
        if (arr.isEmpty())
            arr = doc.object().value(QStringLiteral("comments")).toArray();
        if (arr.isEmpty())
            arr = doc.object().value(QStringLiteral("items")).toArray(); // Nuuru
        if (arr.isEmpty())
            arr = doc.object().value(QStringLiteral("results")).toArray(); // szuru
    }
    for (const QJsonValue &v : arr) {
        if (!v.isObject())
            continue;
        const QJsonObject o = v.toObject();
        if (postId > 0) {
            const qint64 owner =
                o.value(QStringLiteral("post_id")).toVariant().toLongLong();
            if (owner > 0 && owner != postId)
                continue; // unscoped feed: another post's comment
        }
        PostComment c;
        c.author = o.value(QStringLiteral("creator_name")).toString();
        if (c.author.isEmpty())
            c.author = o.value(QStringLiteral("creator")).toString();
        if (c.author.isEmpty())
            c.author = o.value(QStringLiteral("name")).toString();
        if (c.author.isEmpty())
            c.author = o.value(QStringLiteral("author")).toString();
        if (c.author.isEmpty() && o.value(QStringLiteral("author")).isObject())
            c.author = o.value(QStringLiteral("author")).toObject().value(
                QStringLiteral("userName")).toString(); // Nuuru
        if (c.author.isEmpty() && o.value(QStringLiteral("user")).isObject())
            c.author = o.value(QStringLiteral("user")).toObject().value(
                QStringLiteral("name")).toString(); // szuru
        if (c.author.isEmpty())
            c.author = o.value(QStringLiteral("userName")).toString();
        c.body = o.value(QStringLiteral("contentRaw")).toString().trimmed();
        if (c.body.isEmpty())
            c.body = o.value(QStringLiteral("body")).toString().trimmed();
        if (c.body.isEmpty())
            c.body = o.value(QStringLiteral("comment")).toString().trimmed();
        if (c.body.isEmpty())
            c.body = o.value(QStringLiteral("text")).toString().trimmed(); // szuru
        c.date = o.value(QStringLiteral("created_at")).toString();
        if (c.date.isEmpty())
            c.date = o.value(QStringLiteral("createdAt")).toString(); // Nuuru
        if (c.date.isEmpty())
            c.date = o.value(QStringLiteral("created")).toString();
        if (c.date.isEmpty())
            c.date = o.value(QStringLiteral("creationTime")).toString().left(10); // szuru
        if (c.body.isEmpty())
            c.body = o.value(QStringLiteral("contentHtml")).toString().trimmed();
        if (!c.body.isEmpty())
            out.append(c);
    }
    return out;
}

QVector<PostComment> parseCommentary(BooruEngine engine, const QByteArray &body,
                                     bool *ok) {
    QVector<PostComment> out;
    if (ok)
        *ok = false;
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &err);
    if (err.error != QJsonParseError::NoError)
        return out;
    if (ok)
        *ok = true;

    // Philomena: Markdown description on the image/post object.
    if (RemotePosts::isPhilomenaFamily(engine)) {
        const QJsonObject envelope = doc.object();
        const QJsonObject o =
            envelope.value(RemotePosts::philomenaItemKey(engine)).toObject();
        const QString text =
            o.value(QStringLiteral("description")).toString().trimmed();
        if (!text.isEmpty()) {
            PostComment c;
            c.body = text;
            out.append(c);
        }
        return out;
    }

    // HyBooru: Hydrus notes. rect != null are image annotations; rect-null
    // rows are text notes (Danbooru commentary imports carry labels).
    if (engine == BooruEngine::Hybooru) {
        struct Note {
            QString label;
            QString text;
        };
        QVector<Note> notes;
        for (const QJsonValue &v :
             doc.object().value(QStringLiteral("notes")).toArray()) {
            if (!v.isObject())
                continue;
            const QJsonObject n = v.toObject();
            const QJsonValue rect = n.value(QStringLiteral("rect"));
            if (!rect.isNull() && !rect.isUndefined())
                continue; // annotation, handled by the notes overlay
            const QString text =
                n.value(QStringLiteral("note")).toString().trimmed();
            if (text.isEmpty())
                continue;
            notes.append({n.value(QStringLiteral("label"))
                              .toString()
                              .toLower()
                              .trimmed(),
                          text});
        }
        auto hasLabel = [&notes](const QString &needle) {
            for (const Note &n : notes)
                if (n.label.contains(needle))
                    return true;
            return false;
        };
        const bool translated = hasLabel(QStringLiteral("translat"));
        const bool commentary = hasLabel(QStringLiteral("commentary"));
        QStringList texts;
        for (const Note &n : notes) {
            if (translated && !n.label.contains(QStringLiteral("translat")))
                continue;
            if (!translated && commentary &&
                !n.label.contains(QStringLiteral("commentary")))
                continue;
            texts << n.text;
        }
        if (texts.isEmpty()) {
            for (const Note &n : notes)
                texts << n.text; // no commentary labels: keep every note
        }
        if (!texts.isEmpty()) {
            PostComment c;
            c.body = texts.join(QStringLiteral("\n\n"));
            out.append(c);
        }
        return out;
    }

    // Danbooru: {artist_commentary: [...]}, one row per post.
    QJsonArray arr;
    if (doc.isArray())
        arr = doc.array();
    else if (doc.isObject())
        arr = doc.object().value(QStringLiteral("artist_commentary")).toArray();
    for (const QJsonValue &v : arr) {
        if (!v.isObject())
            continue;
        const QJsonObject o = v.toObject();
        PostComment c;
        // Prefer the translation; fall back to the original text.
        QString title = o.value(QStringLiteral("translated_title")).toString();
        QString text =
            o.value(QStringLiteral("translated_description")).toString();
        if (text.trimmed().isEmpty()) {
            title = o.value(QStringLiteral("original_title")).toString();
            text = o.value(QStringLiteral("original_description")).toString();
        }
        // Section header names it; keep the row author empty.
        c.body = (title.trimmed() + (title.trimmed().isEmpty() ? QString()
                                                              : QStringLiteral("\n\n")) +
                  text)
                     .trimmed();
        if (!c.body.isEmpty())
            out.append(c);
    }
    return out;
}

QVector<PostComment> parseCommentary(const QByteArray &body, bool *ok) {
    return parseCommentary(BooruEngine::Danbooru, body, ok);
}

} // namespace PostMeta
