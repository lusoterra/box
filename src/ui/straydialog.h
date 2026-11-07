#pragma once

// Stray collection UI: a downloader/importer dialog (File menu, kept
// deliberately low-key) and the single-box tag editor offered only for
// stray media.
#include <QDialog>
#include <QVector>

#include "post.h"

class QPlainTextEdit;
class QLabel;
class QLineEdit;
class QCheckBox;
class QListWidget;
class QPushButton;

// "File → Download/Add Stray": paste post/media URLs (one per line) to
// fetch, or pick local files to copy in. Emits jobs for MainWindow to run.
class StrayDialog : public QDialog {
    Q_OBJECT
public:
    explicit StrayDialog(bool autoFav, QWidget *parent = nullptr);

signals:
    // Raw URL strings (trimmed, non-empty lines).
    void downloadRequested(const QStringList &urls);
    // Same links, but only recorded in favourites (no media download).
    void favouriteRequested(const QStringList &urls);
    // Local file paths to copy in.
    void importRequested(const QStringList &paths);
    // Auto-favourite toggle flipped (persisted by the owner).
    void autoFavChanged(bool on);

private:
    QPlainTextEdit *m_urls = nullptr;
    QLabel *m_status = nullptr;
    QCheckBox *m_autoFav = nullptr;
};

// Single-box tag editor for one stray file: space-separated tags,
// auto-categorized by the scan pipeline afterwards. Reopen to fix
// anything miscategorized.
class StrayTagDialog : public QDialog {
    Q_OBJECT
public:
    explicit StrayTagDialog(const QString &fileName, const QString &tags,
                            QWidget *parent = nullptr);
    QString tags() const;

private:
    QPlainTextEdit *m_edit = nullptr;
};

// One stray's per-bucket tag set (space-separated, lowercase). The mass
// editor shows tags ordered by category (artist/character/copyright/
// general/meta) and writes both the flat `tags` union and the `tags_*`
// buckets sidecar keys; see MetaParser::parseStray.
struct StrayTagEdit {
    QString absFile;
    QString fileName;
    QString artist;
    QString character;
    QString copyright;
    QString general;
    QString meta;
};

// Mass tag editor for 2+ strays: one window with a file list, per-bucket
// boxes for the selected file, a paste field (whitespace/comma tokens go
// to General) and "Copy from URL" stealing (booru post link resolved via
// StrayResolver, buckets merged). "Copy to all" duplicates the current
// file's buckets to every listed stray.
class StrayMassTagDialog : public QDialog {
    Q_OBJECT
public:
    explicit StrayMassTagDialog(const QVector<Post> &strays,
                                QWidget *parent = nullptr);
    QVector<StrayTagEdit> edits() const;

private:
    void commitCurrent();
    void loadRow(int row);
    QVector<StrayTagEdit> m_edits;
    int m_row = -1;
    QListWidget *m_list = nullptr;
    QPlainTextEdit *m_artist = nullptr;
    QPlainTextEdit *m_character = nullptr;
    QPlainTextEdit *m_copyright = nullptr;
    QPlainTextEdit *m_general = nullptr;
    QPlainTextEdit *m_meta = nullptr;
    QLineEdit *m_paste = nullptr;
    QLineEdit *m_url = nullptr;
    QLabel *m_status = nullptr;
    QPushButton *m_fetch = nullptr;
};
