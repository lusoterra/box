#pragma once

// Portable library backup: one flat CSV row per post (favourite and/or
// downloaded file). The format is deliberately boring so a human can read
// and repair it with any spreadsheet program:
//
//   post_url,file_url,downloaded,favourited,date_added,site,post_id,md5,
//   rating,ext,width,height,tags,local_file
//
// Booleans are `true`/`false` (reading also accepts 1/0, yes/no, y/n, on/
// off). `date_added` is ISO-8601 UTC. `tags` is the usual space-separated
// blob. `local_file` is informational only (path relative to the download
// folder when inside it): imports never trust absolute paths.
//
// Parsing accepts any column order, extra columns, CRLF/LF, quoted fields
// with commas/quotes/newlines, and a UTF-8 BOM. Missing known columns
// default to empty/false. It refuses files with no URL column so a random
// CSV is never mistaken for a backup.
#include <QByteArray>
#include <QString>
#include <QVector>

namespace CsvBackup {

struct Row {
    QString postUrl;   // post page (the stable identity on restore)
    QString fileUrl;   // full-quality media URL
    bool downloaded = false;
    bool favourited = false;
    QString dateAdded; // ISO-8601 UTC, empty when unknown
    QString site;      // display label ("Gelbooru", "Safebooru", ...)
    qint64 postId = 0;
    QString md5;       // content hash when known
    QString rating;    // general|sensitive|questionable|explicit
    QString ext;       // jpg/png/mp4/...
    int width = 0;
    int height = 0;
    QString tags;      // space separated
    QString localFile; // informational, relative to the download folder
};

// Fixed header order; always ends with a newline.
QByteArray write(const QVector<Row> &rows);

// `error` is set only when the data is not a usable backup. An empty file
// or a header-only file is a valid (empty) backup.
QVector<Row> parse(const QByteArray &data, QString *error = nullptr);

// true/1/yes/y/on -> true, false/0/no/n/off/"" -> false; anything else is
// false with ok=false when given.
bool parseBool(const QString &value, bool *ok = nullptr);

// RFC 4180 quoting (only when needed: comma, quote, CR or LF).
QString csvField(const QString &value);

} // namespace CsvBackup
