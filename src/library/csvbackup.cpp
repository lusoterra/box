#include "csvbackup.h"

#include <QHash>
#include <QStringList>

namespace CsvBackup {

namespace {

const char kHeader[] =
    "post_url,file_url,downloaded,favourited,date_added,host,post_id,md5,"
    "rating,ext,width,height,tags,local_file";

// Guards a hostile/mistaken file: a real backup is a few MB. Anything
// larger is truncated instead of OOMing the UI.
constexpr int kMaxBytes = 64 * 1024 * 1024;
constexpr int kMaxRows = 1000000;

QString normalizedHeader(const QString &raw) {
    QString out;
    for (const QChar c : raw.trimmed().toLower())
        if (c.isLetterOrNumber())
            out += c;
    return out;
}

// Typo-tolerant aliases: the header is meant to be obvious, not strict.
QString canonicalColumn(const QString &normalized) {
    static const QHash<QString, QString> map = {
        {QStringLiteral("posturl"), QStringLiteral("post_url")},
        {QStringLiteral("post"), QStringLiteral("post_url")},
        {QStringLiteral("postlink"), QStringLiteral("post_url")},
        {QStringLiteral("postpage"), QStringLiteral("post_url")},
        {QStringLiteral("postid"), QStringLiteral("post_id")},
        {QStringLiteral("id"), QStringLiteral("post_id")},
        {QStringLiteral("fileurl"), QStringLiteral("file_url")},
        {QStringLiteral("file"), QStringLiteral("file_url")},
        {QStringLiteral("mediaurl"), QStringLiteral("file_url")},
        {QStringLiteral("medialink"), QStringLiteral("file_url")},
        {QStringLiteral("downloadurl"), QStringLiteral("file_url")},
        {QStringLiteral("downloaded"), QStringLiteral("downloaded")},
        {QStringLiteral("dowloaded"), QStringLiteral("downloaded")},
        {QStringLiteral("snatched"), QStringLiteral("downloaded")},
        {QStringLiteral("ondisk"), QStringLiteral("downloaded")},
        {QStringLiteral("favourited"), QStringLiteral("favourited")},
        {QStringLiteral("favorited"), QStringLiteral("favourited")},
        {QStringLiteral("favourite"), QStringLiteral("favourited")},
        {QStringLiteral("favorite"), QStringLiteral("favourited")},
        {QStringLiteral("fav"), QStringLiteral("favourited")},
        {QStringLiteral("liked"), QStringLiteral("favourited")},
        {QStringLiteral("dateadded"), QStringLiteral("date_added")},
        {QStringLiteral("added"), QStringLiteral("date_added")},
        {QStringLiteral("addedat"), QStringLiteral("date_added")},
        {QStringLiteral("date"), QStringLiteral("date_added")},
        {QStringLiteral("host"), QStringLiteral("host")},
        {QStringLiteral("hostname"), QStringLiteral("host")},
        {QStringLiteral("domain"), QStringLiteral("host")},
        {QStringLiteral("server"), QStringLiteral("host")},
        // Older backups called it "site" (a display label): same column.
        {QStringLiteral("site"), QStringLiteral("host")},
        {QStringLiteral("booru"), QStringLiteral("host")},
        {QStringLiteral("sitename"), QStringLiteral("host")},
        {QStringLiteral("source"), QStringLiteral("host")},
        {QStringLiteral("md5"), QStringLiteral("md5")},
        {QStringLiteral("rating"), QStringLiteral("rating")},
        {QStringLiteral("ext"), QStringLiteral("ext")},
        {QStringLiteral("width"), QStringLiteral("width")},
        {QStringLiteral("height"), QStringLiteral("height")},
        {QStringLiteral("tags"), QStringLiteral("tags")},
        {QStringLiteral("localfile"), QStringLiteral("local_file")},
        {QStringLiteral("filepath"), QStringLiteral("local_file")},
        {QStringLiteral("path"), QStringLiteral("local_file")},
        {QStringLiteral("filename"), QStringLiteral("local_file")},
    };
    return map.value(normalized);
}

struct SplitResult {
    QVector<QStringList> records;
    QString error;
};

// RFC 4180 plus tolerance for LF-only files and whitespace before an
// opening quote. Field data stays QByteArray until the end so UTF-8 bytes
// are never split on a delimiter byte.
SplitResult splitCsv(const QByteArray &data) {
    SplitResult out;
    QStringList fields;
    QByteArray field;
    bool inQuotes = false;
    bool fieldQuoted = false;
    const QByteArray body = data.startsWith("\xEF\xBB\xBF") ? data.mid(3) : data;

    auto endField = [&] {
        fields << QString::fromUtf8(field);
        field.clear();
        fieldQuoted = false;
    };
    auto endRecord = [&] {
        endField();
        bool any = false;
        for (const QString &f : fields)
            if (!f.isEmpty()) {
                any = true;
                break;
            }
        if (any)
            out.records << fields;
        fields.clear();
    };

    for (int i = 0; i < body.size(); ++i) {
        const char c = body.at(i);
        if (inQuotes) {
            if (c == '"') {
                if (i + 1 < body.size() && body.at(i + 1) == '"') {
                    field += '"';
                    ++i;
                } else {
                    inQuotes = false;
                }
            } else {
                field += c;
            }
            continue;
        }
        if (c == '"' && field.trimmed().isEmpty()) {
            field.clear();
            inQuotes = true;
            fieldQuoted = true;
        } else if (c == ',') {
            endField();
        } else if (c == '\n') {
            if (!fieldQuoted && field.endsWith('\r'))
                field.chop(1);
            endRecord();
        } else {
            field += c;
        }
    }
    if (inQuotes) {
        out.error = QStringLiteral("Unterminated quoted field");
        return out;
    }
    if (!field.isEmpty() || !fields.isEmpty())
        endRecord();
    return out;
}

} // namespace

QString csvField(const QString &value) {
    if (!value.contains(u',') && !value.contains(u'"') &&
        !value.contains(u'\n') && !value.contains(u'\r'))
        return value;
    QString escaped = value;
    escaped.replace(QStringLiteral("\""), QStringLiteral("\"\""));
    return u'"' + escaped + u'"';
}

bool parseBool(const QString &value, bool *ok) {
    const QString v = value.trimmed().toLower();
    if (ok)
        *ok = true;
    if (v == QStringLiteral("1") || v == QStringLiteral("true") ||
        v == QStringLiteral("yes") || v == QStringLiteral("y") ||
        v == QStringLiteral("on"))
        return true;
    if (v.isEmpty() || v == QStringLiteral("0") ||
        v == QStringLiteral("false") || v == QStringLiteral("no") ||
        v == QStringLiteral("n") || v == QStringLiteral("off"))
        return false;
    if (ok)
        *ok = false;
    return false;
}

QByteArray write(const QVector<Row> &rows) {
    QByteArray out;
    out.append(kHeader);
    out.append('\n');
    for (const Row &r : rows) {
        const QStringList cells = {
            csvField(r.postUrl),
            csvField(r.fileUrl),
            r.downloaded ? QStringLiteral("true") : QStringLiteral("false"),
            r.favourited ? QStringLiteral("true") : QStringLiteral("false"),
            csvField(r.dateAdded),
            csvField(r.host.trimmed().toLower()),
            r.postId > 0 ? QString::number(r.postId) : QString(),
            csvField(r.md5),
            csvField(r.rating),
            csvField(r.ext),
            r.width > 0 ? QString::number(r.width) : QString(),
            r.height > 0 ? QString::number(r.height) : QString(),
            csvField(r.tags),
            csvField(r.localFile),
        };
        out.append(cells.join(u',').toUtf8());
        out.append('\n');
    }
    return out;
}

QVector<Row> parse(const QByteArray &data, QString *error) {
    if (error)
        error->clear();
    QVector<Row> out;
    if (data.size() > kMaxBytes) {
        if (error)
            *error = QStringLiteral("CSV backup is larger than 64 MB");
        return out;
    }
    const SplitResult split = splitCsv(data);
    if (!split.error.isEmpty()) {
        if (error)
            *error = split.error;
        return out;
    }
    if (split.records.isEmpty()) {
        if (error)
            *error = QStringLiteral("Empty file");
        return out;
    }
    // Header -> column index. First occurrence wins.
    QHash<QString, int> col;
    const QStringList &header = split.records.first();
    for (int i = 0; i < header.size(); ++i) {
        const QString canonical = canonicalColumn(normalizedHeader(header.at(i)));
        if (!canonical.isEmpty() && !col.contains(canonical))
            col.insert(canonical, i);
    }
    if (!col.contains(QStringLiteral("post_url")) &&
        !col.contains(QStringLiteral("file_url"))) {
        if (error)
            *error = QStringLiteral(
                "Not a box CSV backup (missing post_url/file_url columns)");
        return out;
    }
    auto cell = [&col](const QStringList &fields, const char *name) {
        const auto it = col.constFind(QString::fromLatin1(name));
        if (it == col.constEnd() || it.value() >= fields.size())
            return QString();
        return fields.at(it.value());
    };
    for (int rowIdx = 1; rowIdx < split.records.size(); ++rowIdx) {
        if (out.size() >= kMaxRows)
            break;
        const QStringList &fields = split.records.at(rowIdx);
        Row r;
        r.postUrl = cell(fields, "post_url").trimmed();
        r.fileUrl = cell(fields, "file_url").trimmed();
        r.downloaded = parseBool(cell(fields, "downloaded"));
        r.favourited = parseBool(cell(fields, "favourited"));
        r.dateAdded = cell(fields, "date_added").trimmed();
        r.host = cell(fields, "host").trimmed();
        r.postId = cell(fields, "post_id").trimmed().toLongLong();
        r.md5 = cell(fields, "md5").trimmed().toLower();
        r.rating = cell(fields, "rating").trimmed().toLower();
        r.ext = cell(fields, "ext").trimmed().toLower();
        if (r.ext.startsWith(u'.'))
            r.ext.remove(0, 1);
        r.width = cell(fields, "width").trimmed().toInt();
        r.height = cell(fields, "height").trimmed().toInt();
        r.tags = cell(fields, "tags");
        r.localFile = cell(fields, "local_file").trimmed();
        if (r.postUrl.isEmpty() && r.fileUrl.isEmpty() &&
            r.localFile.isEmpty())
            continue; // nothing identifies the row
        out.append(r);
    }
    return out;
}

} // namespace CsvBackup
