#include "postmeta.h"

#include "powsolver.h"
#include "remotesource.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <QUrlQuery>

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
QNetworkRequest timedRequest(const QUrl &url) {
    QNetworkRequest req(url);
    req.setTransferTimeout(15000);
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
                            qint64 postId, BooruEngine engine) {
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
    QNetworkRequest notesReq = timedRequest(
        PostMeta::notesUrl(engine, baseUrl, postId, m_gelUser, m_gelKey));
    QNetworkRequest commentsReq = timedRequest(
        PostMeta::commentsUrl(engine, baseUrl, postId, m_gelUser, m_gelKey));
    QNetworkRequest commentaryReq = timedRequest(
        PostMeta::commentaryUrl(engine, baseUrl, postId));
    // Unconditional: clears a stale proxy when the entry drops it.
    m_nam->setProxy(RemotePosts::proxyFromSpec(
        RemotePosts::proxyForUrl(baseUrl)));
    if (engine == BooruEngine::Szurubooru) {
        // Szurubooru's API answers 406 without an explicit Accept header.
        notesReq.setRawHeader("Accept", "application/json");
        commentsReq.setRawHeader("Accept", "application/json");
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
    if (!notesReq.url().isEmpty()) {
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
                if (r->error() == QNetworkReply::NoError && http / 100 == 2)
                    m_notes = PostMeta::parseNotes(body, &ok);
                else if (r->error() != QNetworkReply::NoError)
                    m_notesErr = r->errorString();
                else
                    m_notesErr = tr("HTTP %1").arg(http);
                r->deleteLater();
            }
            m_notesOk = ok;
            if (!ok && maybePowRetry(body, http))
                return; // native re-solve in flight; results land as new done
            m_notesDone = true;
            finishPart();
        });
    }
    if (!commentsReq.url().isEmpty()) {
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
                } else if (r->error() == QNetworkReply::NoError && http / 100 == 2)
                    m_comments = PostMeta::parseComments(body, &ok);
                else if (r->error() != QNetworkReply::NoError)
                    m_commentsErr = r->errorString();
                else
                    m_commentsErr = tr("HTTP %1").arg(http);
                r->deleteLater();
            }
            m_commentsOk = ok;
            if (!ok && maybePowRetry(body, http))
                return; // native re-solve in flight; results land as new done
            m_commentsDone = true;
            finishPart();
        });
    }
    if (!commentaryReq.url().isEmpty()) {
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
                if (r->error() == QNetworkReply::NoError && http / 100 == 2)
                    m_commentary = PostMeta::parseCommentary(body, &ok);
                else if (r->error() != QNetworkReply::NoError)
                    m_commentaryErr = r->errorString();
                else
                    m_commentaryErr = tr("HTTP %1").arg(http);
                r->deleteLater();
            }
            m_commentaryOk = ok;
            if (!ok && maybePowRetry(body, http))
                return;
            m_commentaryDone = true;
            finishPart();
        });
    }
    finishPart(); // in case every URL was empty
}

void PostMetaFetcher::cancel() {
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

void PostMetaFetcher::finishPart() {
    if (!m_notesDone || !m_commentsDone || !m_commentaryDone)
        return;
    QString err = m_notesErr;
    if (err.isEmpty())
        err = m_commentsErr;
    // Commentary is optional enrichment: a failure there must not hide
    // notes/comments that loaded fine.
    emit done(m_site, m_postId, m_notes, m_comments, m_commentary,
              m_notesOk && m_commentsOk, err);
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
    if (engine == BooruEngine::Szurubooru || engine == BooruEngine::Shimmie ||
        engine == BooruEngine::Hybooru || engine == BooruEngine::Nuuru ||
        engine == BooruEngine::Derpibooru || engine == BooruEngine::Twibooru)
        return {}; // no notes endpoint on these engines
    if (engine == BooruEngine::Danbooru) {
        QUrl url(baseUrl + QStringLiteral("/notes.json"));
        QUrlQuery q;
        q.addQueryItem(QStringLiteral("search[post_id]"), id);
        q.addQueryItem(QStringLiteral("limit"), QStringLiteral("100"));
        url.setQuery(q);
        return url;
    }
    if (engine == BooruEngine::Gelbooru || engine == BooruEngine::Safebooru) {
        QUrl url(RemotePosts::apiBaseUrl(engine, baseUrl) +
                 QStringLiteral("/index.php"));
        QUrlQuery q;
        q.addQueryItem(QStringLiteral("page"), QStringLiteral("dapi"));
        q.addQueryItem(QStringLiteral("s"), QStringLiteral("note"));
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
    // Moebooru family.
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
    if (engine == BooruEngine::Shimmie || engine == BooruEngine::Hybooru ||
        engine == BooruEngine::Derpibooru || engine == BooruEngine::Twibooru)
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
    if (engine == BooruEngine::Danbooru) {
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
    return QUrl(baseUrl + QStringLiteral("/comment.json?post_id=") + id);
}

QUrl commentaryUrl(BooruEngine engine, const QString &baseUrl, qint64 postId) {
    if (engine != BooruEngine::Danbooru)
        return {};
    QUrl url(baseUrl + QStringLiteral("/artist_commentary.json"));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("search[post_id]"),
                   QString::number(postId));
    q.addQueryItem(QStringLiteral("limit"), QStringLiteral("1"));
    url.setQuery(q);
    return url;
}

QVector<PostNote> parseNotes(const QByteArray &body, bool *ok) {
    QVector<PostNote> out;
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &err);
    if (err.error != QJsonParseError::NoError) {
        if (ok)
            *ok = false; // challenge HTML, wall pages: not definitive
        return out;
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

QVector<PostComment> parseComments(const QByteArray &body, bool *ok) {
    QVector<PostComment> out;
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &err);
    if (err.error != QJsonParseError::NoError) {
        if (ok)
            *ok = false; // challenge HTML, wall pages: not definitive
        return out;
    }
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

QVector<PostComment> parseCommentary(const QByteArray &body, bool *ok) {
    QVector<PostComment> out;
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &err);
    if (err.error != QJsonParseError::NoError) {
        if (ok)
            *ok = false;
        return out;
    }
    if (ok)
        *ok = true;
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

} // namespace PostMeta
