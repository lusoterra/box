#include "sharecatalog.h"

#include "queryor.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonValue>
#include <QRandomGenerator>
#include <QSet>
#include <QUrl>

#include <algorithm>
#include <cstring>

namespace {

QString joined(const QStringList &tokens) {
    return tokens.join(QChar::Space);
}

void mergeBlob(QString &dst, const QString &src) {
    if (src.trimmed().isEmpty())
        return;
    QStringList tokens = dst.split(QChar::Space, Qt::SkipEmptyParts);
    QSet<QString> seen(tokens.begin(), tokens.end());
    for (const QString &t :
         src.split(QChar::Space, Qt::SkipEmptyParts)) {
        if (seen.contains(t))
            continue;
        seen.insert(t);
        tokens << t;
    }
    dst = joined(tokens);
}

void mergeCustom(QMap<QString, QStringList> &dst,
                 const QMap<QString, QStringList> &src) {
    for (auto it = src.begin(); it != src.end(); ++it) {
        QStringList &list = dst[it.key()];
        for (const QString &tag : it.value()) {
            if (!list.contains(tag))
                list << tag;
        }
    }
}

QString normalizedUrl(const QString &raw) {
    const QUrl u(raw.trimmed());
    if (!u.isValid() || u.scheme().isEmpty() || u.host().isEmpty())
        return raw.trimmed();
    QString out = u.scheme().toLower() + QStringLiteral("://") +
                  u.host().toLower();
    if (u.port() > 0)
        out += u'_' + QString::number(u.port());
    out += u.path();
    const QString query = u.query(QUrl::FullyEncoded);
    if (!query.isEmpty())
        out += u'?' + query;
    return out;
}

bool ratingToken(const QString &tok) {
    const QString low = tok.toLower();
    return low.startsWith(QStringLiteral("rating:")) ||
           low.startsWith(QStringLiteral("safety:"));
}

bool ratingMatches(const QString &postRating, const QString &tok) {
    QString want = tok.mid(tok.indexOf(u':') + 1).toLower();
    if (want == QStringLiteral("safe") || want == QStringLiteral("g"))
        want = QStringLiteral("general");
    else if (want == QStringLiteral("s"))
        want = QStringLiteral("sensitive");
    else if (want == QStringLiteral("q"))
        want = QStringLiteral("questionable");
    else if (want == QStringLiteral("e"))
        want = QStringLiteral("explicit");
    return postRating.compare(want, Qt::CaseInsensitive) == 0;
}

bool sortToken(const QString &tok) {
    const QString low = tok.toLower();
    return low.startsWith(QStringLiteral("order:")) ||
           low.startsWith(QStringLiteral("sort:"));
}

QString tokenValue(const QString &tok) {
    const qsizetype colon = tok.indexOf(u':');
    return colon < 0 ? QString() : tok.mid(colon + 1).toLower();
}

QStringList searchableTags(const Post &p) {
    QStringList tags = p.tags.split(QChar::Space, Qt::SkipEmptyParts);
    if (!tags.isEmpty())
        return tags;
    const auto add = [&tags](const QString &blob) {
        for (const QString &t :
             blob.split(QChar::Space, Qt::SkipEmptyParts))
            if (!tags.contains(t))
                tags << t;
    };
    add(p.tagsArtist);
    add(p.tagsCharacter);
    add(p.tagsCopyright);
    add(p.tagsGeneral);
    add(p.tagsMeta);
    add(p.untyped);
    for (auto it = p.customTags.begin(); it != p.customTags.end(); ++it) {
        for (const QString &t : it.value()) {
            const QString full = it.key() + u':' + t;
            if (!tags.contains(t))
                tags << t;
            if (!tags.contains(full))
                tags << full;
        }
    }
    return tags;
}

bool matchesItem(const ShareItem &item, const QStringList &inc,
                 const QStringList &exc) {
    const Post &p = item.post;
    const QStringList tags = searchableTags(p);
    QList<QSet<QString>> ftInc, ftExc;
    const auto addFiletype = [](QList<QSet<QString>> &dst, const QString &tok) {
        dst.append(filetypeExtSet(tok));
    };
    for (const QString &x : inc) {
        const QString low = x.toLower();
        if (low == QStringLiteral("animated")) {
            addFiletype(ftInc, mediaAnimatedToken());
        } else if (low == QStringLiteral("animated_gif")) {
            addFiletype(ftInc, mediaGifToken());
        } else if (low == QStringLiteral("video")) {
            addFiletype(ftInc, mediaVideoToken());
        } else if (isFiletypeToken(x)) {
            addFiletype(ftInc, x);
        }
    }
    for (const QString &x : exc) {
        const QString low = x.toLower();
        if (low == QStringLiteral("animated")) {
            addFiletype(ftExc, mediaAnimatedToken());
        } else if (low == QStringLiteral("animated_gif")) {
            addFiletype(ftExc, mediaGifToken());
        } else if (low == QStringLiteral("video")) {
            addFiletype(ftExc, mediaVideoToken());
        } else if (isFiletypeToken(x)) {
            addFiletype(ftExc, x);
        }
    }
    if (!filetypeExtOk(p.ext, ftInc, ftExc))
        return false;

    const auto matchesOne = [&](const QString &x) {
        const QString low = x.toLower();
        if (ratingToken(x))
            return ratingMatches(p.rating, x);
        if (isFiletypeToken(x) || low == QStringLiteral("animated") ||
            low == QStringLiteral("animated_gif") ||
            low == QStringLiteral("video"))
            return true; // handled above
        if (low.startsWith(QStringLiteral("id:")))
            return QString::number(item.id) == tokenValue(x);
        if (low.startsWith(QStringLiteral("md5:")))
            return p.md5.compare(tokenValue(x), Qt::CaseInsensitive) == 0;
        if (low.startsWith(QStringLiteral("sha256:")))
            return p.sha256.compare(tokenValue(x), Qt::CaseInsensitive) == 0;
        return tags.contains(x);
    };
    for (const QString &x : inc) {
        if (!matchesOne(x))
            return false;
    }
    for (const QString &x : exc) {
        if (matchesOne(x))
            return false;
    }
    return true;
}

int bucketCategory(const QString &bucket) {
    if (bucket == QStringLiteral("artist"))
        return 1;
    if (bucket == QStringLiteral("copyright"))
        return 3;
    if (bucket == QStringLiteral("character"))
        return 4;
    if (bucket == QStringLiteral("meta"))
        return 5;
    return 0;
}

QString namespaceBucket(const QString &ns) {
    const QString n = ns.toLower();
    if (n == QStringLiteral("creator") || n == QStringLiteral("artist") ||
        n == QStringLiteral("person"))
        return QStringLiteral("artist");
    if (n == QStringLiteral("character") || n == QStringLiteral("char"))
        return QStringLiteral("character");
    if (n == QStringLiteral("series") || n == QStringLiteral("copyright") ||
        n == QStringLiteral("studio") || n == QStringLiteral("franchise"))
        return QStringLiteral("copyright");
    if (n == QStringLiteral("meta") || n == QStringLiteral("title"))
        return QStringLiteral("meta");
    return QStringLiteral("general");
}

} // namespace

qint64 ShareCatalog::stableId(const QString &key) {
    const QByteArray h =
        QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha1);
    qint64 id = 0;
    memcpy(&id, h.constData(), sizeof(id));
    // JSON numbers reach clients through a double in many stacks (box's
    // own QJson included); keep the id inside 53 bits so `/posts/<id>`
    // round-trips exactly. Still astronomically unlikely to collide.
    return id & 0x1fffffffffffffLL;
}

QStringList ShareCatalog::keysFor(const Post &post, const QString &localPath,
                                  const QString &remoteMediaUrl) const {
    QStringList keys;
    const QString site =
        !post.siteKey.isEmpty() ? post.siteKey : post.siteName;
    if (post.id > 0 && !site.isEmpty())
        keys << QStringLiteral("site#%1#%2")
                    .arg(site.toLower())
                    .arg(post.id);
    if (!post.sha256.isEmpty())
        keys << QStringLiteral("sha256#%1").arg(post.sha256.toLower());
    if (!post.md5.isEmpty())
        keys << QStringLiteral("md5#%1").arg(post.md5.toLower());
    if (!localPath.isEmpty()) {
        QString canon = QFileInfo(localPath).canonicalFilePath();
        if (canon.isEmpty())
            canon = QFileInfo(localPath).absoluteFilePath();
        keys << QStringLiteral("path#%1").arg(canon);
    }
    if (!remoteMediaUrl.isEmpty())
        keys << QStringLiteral("url#%1").arg(normalizedUrl(remoteMediaUrl));
    if (!post.fileUrl.isEmpty())
        keys << QStringLiteral("url#%1").arg(normalizedUrl(post.fileUrl));
    return keys;
}

void ShareCatalog::registerKeys(int index, const QStringList &keys) {
    for (const QString &key : keys) {
        if (!key.isEmpty() && !m_byKey.contains(key))
            m_byKey.insert(key, index);
    }
}

void ShareCatalog::mergeInto(ShareItem &dst, const Post &post,
                             const QString &localPath,
                             const QString &remoteMediaUrl) {
    Post &p = dst.post;
    if (dst.localPath.isEmpty() && !localPath.isEmpty()) {
        dst.localPath = localPath;
        p.absFile = localPath;
        p.downloaded = true;
    }
    if (dst.remoteMediaUrl.isEmpty() && !remoteMediaUrl.isEmpty())
        dst.remoteMediaUrl = remoteMediaUrl;
    if (dst.remotePreviewUrl.isEmpty() && !post.previewUrl.isEmpty())
        dst.remotePreviewUrl = post.previewUrl;

    mergeBlob(p.tags, post.tags);
    mergeBlob(p.tagsArtist, post.tagsArtist);
    mergeBlob(p.tagsCharacter, post.tagsCharacter);
    mergeBlob(p.tagsCopyright, post.tagsCopyright);
    mergeBlob(p.tagsGeneral, post.tagsGeneral);
    mergeBlob(p.tagsMeta, post.tagsMeta);
    mergeBlob(p.untyped, post.untyped);
    mergeCustom(p.customTags, post.customTags);

    if (p.rating.isEmpty() || p.rating == QStringLiteral("general")) {
        if (!post.rating.isEmpty())
            p.rating = post.rating;
    }
    if (p.md5.isEmpty())
        p.md5 = post.md5;
    if (p.sha256.isEmpty())
        p.sha256 = post.sha256;
    if (p.previewUrl.isEmpty())
        p.previewUrl = post.previewUrl;
    if (p.sampleUrl.isEmpty())
        p.sampleUrl = post.sampleUrl;
    if (p.fileUrl.isEmpty())
        p.fileUrl = post.fileUrl;
    if (p.postUrl.isEmpty())
        p.postUrl = post.postUrl;
    if (p.source.isEmpty())
        p.source = post.source;
    if (p.date.isEmpty())
        p.date = post.date;
    if (p.siteName.isEmpty())
        p.siteName = post.siteName;
    if (p.siteKey.isEmpty())
        p.siteKey = post.siteKey;
    if (p.width == 0)
        p.width = post.width;
    if (p.height == 0)
        p.height = post.height;
    if (p.score == 0)
        p.score = post.score;
    p.hasMeta = p.hasMeta || post.hasMeta;
    p.hasNotes = p.hasNotes || post.hasNotes;
    p.hasComments = p.hasComments || post.hasComments;
    if (p.commentCount == 0)
        p.commentCount = post.commentCount;
    if (p.rawJson.isEmpty())
        p.rawJson = post.rawJson;
}

bool ShareCatalog::add(const Post &post, const QString &localPathIn,
                       const QString &remoteMediaUrlIn) {
    QString localPath = localPathIn;
    if (localPath.isEmpty() && !post.absFile.isEmpty())
        localPath = post.absFile;
    if (!localPath.isEmpty() && !QFileInfo::exists(localPath))
        localPath.clear();

    QString remote = remoteMediaUrlIn;
    if (remote.isEmpty()) {
        remote = post.fileUrl;
        if (remote.isEmpty())
            remote = post.sampleUrl;
    }
    if (localPath.isEmpty() && remote.isEmpty())
        return false; // no playable target

    const QStringList keys = keysFor(post, localPath, remote);
    if (keys.isEmpty())
        return false;

    int index = -1;
    for (const QString &key : keys) {
        const auto it = m_byKey.constFind(key);
        if (it != m_byKey.constEnd()) {
            index = it.value();
            break;
        }
    }
    if (index >= 0) {
        ShareItem &item = m_items[index];
        mergeInto(item, post, localPath, remote);
        registerKeys(index, keys);
        return true;
    }

    ShareItem item;
    item.id = stableId(keys.first());
    if (item.id == 0)
        item.id = 1;
    int salt = 0;
    while (m_byId.contains(item.id)) {
        item.id = stableId(keys.first() + QStringLiteral("#") +
                           QString::number(++salt));
        if (item.id == 0)
            item.id = ++salt;
    }
    item.post = post;
    item.localPath = localPath;
    item.remoteMediaUrl = remote;
    item.remotePreviewUrl = post.previewUrl;
    if (!localPath.isEmpty()) {
        item.post.absFile = localPath;
        item.post.downloaded = true;
    }
    m_items.append(item);
    const int newIndex = static_cast<int>(m_items.size()) - 1;
    m_byId.insert(item.id, newIndex);
    registerKeys(newIndex, keys);
    return true;
}

void ShareCatalog::clear() {
    m_items.clear();
    m_byKey.clear();
    m_byId.clear();
}

const ShareItem *ShareCatalog::byId(qint64 id) const {
    const auto it = m_byId.constFind(id);
    return it == m_byId.constEnd() ? nullptr : &m_items.at(it.value());
}

QVector<const ShareItem *> ShareCatalog::search(const QString &tags, int page,
                                                int limit, int *total) const {
    const QueryOr::ParsedQuery pq = QueryOr::parse(tags);
    QStringList inc, exc;
    for (const QString &tok : pq.andTokens) {
        if (sortToken(tok))
            continue;
        if (tok.startsWith(u'-') && tok.size() > 1)
            exc << tok.mid(1);
        else if (!tok.isEmpty())
            inc << tok;
    }
    QVector<const ShareItem *> out;
    for (const ShareItem &item : m_items) {
        if (!matchesItem(item, inc, exc))
            continue;
        const QStringList tagList = searchableTags(item.post);
        const QSet<QString> tagSet(tagList.begin(), tagList.end());
        if (!QueryOr::groupsMatch(tagSet, pq.groups))
            continue;
        out.append(&item);
    }

    QString sort;
    for (const QString &tok : pq.andTokens) {
        if (sortToken(tok)) {
            sort = tokenValue(tok);
            break;
        }
    }
    if (sort == QStringLiteral("score_asc") ||
        sort == QStringLiteral("score-asc")) {
        std::stable_sort(out.begin(), out.end(),
                         [](const ShareItem *a, const ShareItem *b) {
                             return a->post.score < b->post.score;
                         });
    } else if (sort == QStringLiteral("score") ||
               sort == QStringLiteral("score_desc") ||
               sort == QStringLiteral("score-desc")) {
        std::stable_sort(out.begin(), out.end(),
                         [](const ShareItem *a, const ShareItem *b) {
                             return a->post.score > b->post.score;
                         });
    } else if (sort == QStringLiteral("id") ||
               sort == QStringLiteral("id_desc") ||
               sort == QStringLiteral("id-desc")) {
        std::stable_sort(out.begin(), out.end(),
                         [](const ShareItem *a, const ShareItem *b) {
                             return a->id > b->id;
                         });
    } else if (sort == QStringLiteral("id_asc") ||
               sort == QStringLiteral("id-asc")) {
        std::stable_sort(out.begin(), out.end(),
                         [](const ShareItem *a, const ShareItem *b) {
                             return a->id < b->id;
                         });
    } else if (sort == QStringLiteral("random")) {
        std::shuffle(out.begin(), out.end(), *QRandomGenerator::global());
    } else {
        // Default: newest first, then widest id as a stable tiebreaker.
        std::stable_sort(out.begin(), out.end(),
                         [](const ShareItem *a, const ShareItem *b) {
                             if (a->post.date != b->post.date)
                                 return a->post.date > b->post.date;
                             return a->id > b->id;
                         });
    }

    if (total)
        *total = static_cast<int>(out.size());
    const int per = qBound(1, limit, 200);
    const int p = qMax(1, page);
    const int start = (p - 1) * per;
    if (start >= out.size())
        return {};
    return out.mid(start, per);
}

QJsonObject ShareCatalog::postJson(const ShareItem &item,
                                   const QString &baseUrl) const {
    const Post &p = item.post;
    QJsonObject o;
    o[QStringLiteral("id")] = item.id;
    o[QStringLiteral("created_at")] =
        p.date.isEmpty() ? QStringLiteral("1970-01-01T00:00:00Z") : p.date;
    o[QStringLiteral("uploader_id")] = 0;
    o[QStringLiteral("uploader_name")] = QStringLiteral("box");
    o[QStringLiteral("score")] = p.score;
    o[QStringLiteral("source")] =
        !p.source.isEmpty() ? p.source : p.postUrl;
    o[QStringLiteral("md5")] = p.md5;
    if (!p.sha256.isEmpty())
        o[QStringLiteral("sha256")] = p.sha256;
    o[QStringLiteral("rating")] =
        p.rating.isEmpty() ? QStringLiteral("general") : p.rating;
    o[QStringLiteral("image_width")] = p.width;
    o[QStringLiteral("image_height")] = p.height;
    QString ext = p.ext;
    if (ext.isEmpty() && !item.localPath.isEmpty())
        ext = QFileInfo(item.localPath).suffix().toLower();
    if (ext.isEmpty() && !item.remoteMediaUrl.isEmpty())
        ext = QFileInfo(QUrl(item.remoteMediaUrl).path()).suffix().toLower();
    o[QStringLiteral("file_ext")] = ext;
    const QFileInfo local(item.localPath);
    o[QStringLiteral("file_size")] =
        (!item.localPath.isEmpty() && local.exists()) ? local.size() : 0;

    const QString fileUrl =
        baseUrl + QStringLiteral("/share/file/") + QString::number(item.id);
    const QString previewUrl = previewUrlFor(item, baseUrl);
    o[QStringLiteral("file_url")] = fileUrl;
    o[QStringLiteral("large_file_url")] = fileUrl;
    o[QStringLiteral("sample_url")] = fileUrl;
    o[QStringLiteral("preview_file_url")] = previewUrl;
    o[QStringLiteral("preview_url")] = previewUrl;
    o[QStringLiteral("post_url")] =
        baseUrl + QStringLiteral("/posts/") + QString::number(item.id);

    QStringList artist, character, copyright, general, meta;
    QSet<QString> seen;
    const auto addTokens = [&seen](QStringList &dst, const QString &blob) {
        for (const QString &t :
             blob.split(QChar::Space, Qt::SkipEmptyParts)) {
            if (seen.contains(t))
                continue;
            seen.insert(t);
            dst << t;
        }
    };
    addTokens(artist, p.tagsArtist);
    addTokens(character, p.tagsCharacter);
    addTokens(copyright, p.tagsCopyright);
    addTokens(general, p.tagsGeneral);
    addTokens(general, p.untyped);
    addTokens(meta, p.tagsMeta);
    for (auto it = p.customTags.begin(); it != p.customTags.end(); ++it) {
        const QString bucket = namespaceBucket(it.key());
        const bool mapped = bucket != QStringLiteral("general");
        for (const QString &t : it.value()) {
            const QString token =
                mapped ? t : it.key() + u':' + t;
            if (seen.contains(token))
                continue;
            seen.insert(token);
            if (bucket == QStringLiteral("artist"))
                artist << token;
            else if (bucket == QStringLiteral("character"))
                character << token;
            else if (bucket == QStringLiteral("copyright"))
                copyright << token;
            else if (bucket == QStringLiteral("meta"))
                meta << token;
            else
                general << token;
        }
    }
    QStringList all = artist + character + copyright + general + meta;
    all.removeDuplicates();
    if (all.isEmpty() && !p.tags.trimmed().isEmpty()) {
        general = p.tags.split(QChar::Space, Qt::SkipEmptyParts);
        general.removeDuplicates();
        all = general;
    }
    o[QStringLiteral("tag_string")] = all.join(QChar::Space);
    o[QStringLiteral("tag_string_artist")] = artist.join(QChar::Space);
    o[QStringLiteral("tag_string_character")] = character.join(QChar::Space);
    o[QStringLiteral("tag_string_copyright")] = copyright.join(QChar::Space);
    o[QStringLiteral("tag_string_general")] = general.join(QChar::Space);
    o[QStringLiteral("tag_string_meta")] = meta.join(QChar::Space);
    o[QStringLiteral("tag_count")] = static_cast<int>(all.size());
    o[QStringLiteral("tag_count_general")] = static_cast<int>(general.size());
    o[QStringLiteral("tag_count_artist")] = static_cast<int>(artist.size());
    o[QStringLiteral("tag_count_character")] =
        static_cast<int>(character.size());
    o[QStringLiteral("tag_count_copyright")] =
        static_cast<int>(copyright.size());
    o[QStringLiteral("tag_count_meta")] = static_cast<int>(meta.size());
    o[QStringLiteral("has_notes")] = p.hasNotes;
    o[QStringLiteral("has_comments")] = p.hasComments;
    o[QStringLiteral("is_flagged")] = false;
    o[QStringLiteral("is_pending")] = false;
    o[QStringLiteral("is_deleted")] = false;
    return o;
}

QJsonArray ShareCatalog::postsJson(const QVector<const ShareItem *> &items,
                                   const QString &baseUrl) const {
    QJsonArray arr;
    for (const ShareItem *item : items) {
        if (item)
            arr.append(postJson(*item, baseUrl));
    }
    return arr;
}

QString ShareCatalog::previewUrlFor(const ShareItem &item,
                                    const QString &baseUrl) {
    QString ext = item.post.ext;
    if (ext.isEmpty() && !item.localPath.isEmpty())
        ext = QFileInfo(item.localPath).suffix().toLower();
    if (ext.isEmpty() && !item.remoteMediaUrl.isEmpty())
        ext = QFileInfo(QUrl(item.remoteMediaUrl).path()).suffix().toLower();
    if (!item.localPath.isEmpty()) {
        if (isVideoExt(ext) && !item.post.previewUrl.isEmpty())
            return item.post.previewUrl;
        return baseUrl + QStringLiteral("/share/thumb/") +
               QString::number(item.id);
    }
    if (!item.post.previewUrl.isEmpty())
        return item.post.previewUrl;
    if (!item.remotePreviewUrl.isEmpty())
        return item.remotePreviewUrl;
    return baseUrl + QStringLiteral("/share/thumb/") +
           QString::number(item.id);
}

QJsonArray ShareCatalog::tagsJson(const QString &prefix, int limit) const {
    QHash<QString, QPair<int, int>> counts; // tag -> (post count, category)
    for (const ShareItem &item : m_items) {
        QHash<QString, int> cat;
        const auto addBucket = [&cat](const QString &blob,
                                      const QString &bucket) {
            const int c = bucketCategory(bucket);
            for (const QString &t :
                 blob.split(QChar::Space, Qt::SkipEmptyParts))
                cat.insert(t, c);
        };
        addBucket(item.post.tagsArtist, QStringLiteral("artist"));
        addBucket(item.post.tagsCharacter, QStringLiteral("character"));
        addBucket(item.post.tagsCopyright, QStringLiteral("copyright"));
        addBucket(item.post.tagsMeta, QStringLiteral("meta"));
        addBucket(item.post.tagsGeneral, QStringLiteral("general"));
        addBucket(item.post.untyped, QStringLiteral("general"));
        for (auto it = item.post.customTags.begin();
             it != item.post.customTags.end(); ++it) {
            const QString bucket = namespaceBucket(it.key());
            const int c = bucketCategory(bucket);
            for (const QString &t : it.value()) {
                cat.insert(t, c);
                cat.insert(it.key() + u':' + t, c);
            }
        }
        QSet<QString> seen;
        QStringList tokens =
            item.post.tags.split(QChar::Space, Qt::SkipEmptyParts);
        if (tokens.isEmpty()) {
            for (auto it = cat.begin(); it != cat.end(); ++it)
                tokens << it.key();
        }
        for (const QString &t : tokens) {
            if (seen.contains(t))
                continue;
            seen.insert(t);
            const auto it = cat.constFind(t);
            const int c = it == cat.constEnd() ? 0 : it.value();
            QPair<int, int> &row = counts[t];
            ++row.first;
            row.second = qMax(row.second, c);
        }
    }

    QString q = prefix.trimmed();
    if (q.endsWith(u'*'))
        q.chop(1);
    q.remove(u'%');
    const int per = qBound(1, limit, 200);
    QVector<QPair<QString, QPair<int, int>>> rows;
    rows.reserve(counts.size());
    for (auto it = counts.begin(); it != counts.end(); ++it) {
        if (!q.isEmpty() && !it.key().startsWith(q, Qt::CaseInsensitive))
            continue;
        rows.append({it.key(), it.value()});
    }
    std::stable_sort(rows.begin(), rows.end(),
                     [](const auto &a, const auto &b) {
                         if (a.second.first != b.second.first)
                             return a.second.first > b.second.first;
                         return a.first.compare(b.first, Qt::CaseInsensitive) <
                                0;
                     });
    if (rows.size() > per)
        rows.resize(per);
    QJsonArray arr;
    for (const auto &row : rows) {
        QJsonObject o;
        o[QStringLiteral("id")] =
            stableId(QStringLiteral("tag#") + row.first);
        o[QStringLiteral("name")] = row.first;
        o[QStringLiteral("post_count")] = row.second.first;
        o[QStringLiteral("category")] = row.second.second;
        static const char *names[] = {"general", "artist", "unknown",
                                      "copyright", "character",
                                      "meta"};
        o[QStringLiteral("category_name")] =
            row.second.second >= 0 && row.second.second < 6
                ? QString::fromLatin1(names[row.second.second])
                : QStringLiteral("general");
        arr.append(o);
    }
    return arr;
}

int ShareCatalog::count(const QString &tags) const {
    int total = 0;
    search(tags, 1, 200, &total);
    return total;
}
