#pragma once

// Notes (annotations) + comments for remote posts. Per-engine endpoints:
//   Danbooru:  /notes.json?search[post_id]=, /comments.json?search[post_id]=
//   Gelbooru:  dapi s=note|s=comment, post_id= (creds attached when set)
//   Moebooru:  /note.json?post_id=, /comment.json?post_id=
// Failures are silent (sections just stay hidden); results are cached per
// session so back/forth doesn't refetch.
#include <QObject>
#include <QString>
#include <QVector>

#include "remotesource.h" // BooruEngine: custom instances share family endpoints

struct PostNote {
    int x = 0, y = 0, w = 0, h = 0;
    QString body;
};

struct PostComment {
    QString author;
    QString body;
    QString date;
    QString avatarUrl; // empty: no engine exposes one yet (initials used)
};

class PostMetaFetcher : public QObject {
    Q_OBJECT
public:
    explicit PostMetaFetcher(QObject *parent = nullptr);
    void setCreds(const QString &danUser, const QString &danKey,
                  const QString &gelUser, const QString &gelKey);
    void fetch(const QString &siteKey, const QString &baseUrl, qint64 postId,
               BooruEngine engine);
    void cancel();

signals:
    // ok=false: the fetch failed (network, HTTP error, or an unparseable
    // body like a challenge page) — caches must NOT treat the empties as
    // definitive, and the UI should offer a retry instead of silence.
    void done(const QString &siteKey, qint64 postId, QVector<PostNote> notes,
              QVector<PostComment> comments, bool ok);

private:
    void finishPart();
    class QNetworkAccessManager *m_nam;
    class QNetworkReply *m_notesReply = nullptr;
    class QNetworkReply *m_commentsReply = nullptr;
    QString m_site;
    qint64 m_postId = 0;
    QVector<PostNote> m_notes;
    QVector<PostComment> m_comments;
    bool m_notesDone = true;
    bool m_commentsDone = true;
    bool m_notesOk = true;
    bool m_commentsOk = true;
    QString m_danUser, m_danKey, m_gelUser, m_gelKey;
};

namespace PostMeta {
QUrl notesUrl(BooruEngine engine, const QString &baseUrl, qint64 postId,
              const QString &gelUser = {}, const QString &gelKey = {});
QUrl commentsUrl(BooruEngine engine, const QString &baseUrl, qint64 postId,
                 const QString &gelUser = {}, const QString &gelKey = {});
QVector<PostNote> parseNotes(const QByteArray &body, bool *ok = nullptr);
QVector<PostComment> parseComments(const QByteArray &body, bool *ok = nullptr);
} // namespace PostMeta
