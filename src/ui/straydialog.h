#pragma once

// Stray collection UI: a downloader/importer dialog (File menu, kept
// deliberately low-key) and the single-box tag editor offered only for
// stray media.
#include <QDialog>
#include <QVector>

#include "hydrus.h"
#include "post.h"

class QPlainTextEdit;
class QLabel;
class QLineEdit;
class QCheckBox;
class QListWidget;
class QPushButton;
class QProgressBar;

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

// Progress view for one system gallery-dl flight (Pixiv/Fanbox/Pawchive
// galleries, …): live log, current item, Cancel. Fed by GalleryDlRunner
// signals; the owner reports the finish summary.
class GalleryDlDialog : public QDialog {
    Q_OBJECT
public:
    explicit GalleryDlDialog(const QString &destDir, QWidget *parent = nullptr);

public slots:
    void appendLog(const QString &line);
    void showCurrent(const QString &current);
    void showFinished(bool ok, int filesFound, const QString &error);
    // Hide "Open folder"/"Browse in app" (Hydrus staging is temporary and
    // imported into the Hydrus tab, not _Stray).
    void setShowFolderActions(bool on);

signals:
    void cancelRequested();
    void browseRequested(const QString &dir);

private:
    QLabel *m_current = nullptr;
    QListWidget *m_log = nullptr;
    QProgressBar *m_bar = nullptr;
    QPushButton *m_cancelBtn = nullptr;
    QPushButton *m_closeBtn = nullptr;
    QPushButton *m_openFolderBtn = nullptr;
    QPushButton *m_browseBtn = nullptr;
    QString m_destDir;
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

// Single-box tag editor for one stray file: space-separated tags,
// auto-categorized by the scan pipeline afterwards. Reopen to fix
// anything miscategorized.
class StrayTagDialog : public QDialog {
    Q_OBJECT
public:
    explicit StrayTagDialog(const QString &fileName, const QString &tags,
                            const Hydrus::Config &hydrus,
                            const StrayTagEdit &existing,
                            QWidget *parent = nullptr);
    QString tags() const;
    // True when a Hydrus fetch populated the box: saving then writes the
    // fetched namespace categories (plus any manual additions as General)
    // instead of leaving everything untyped for the online classifier.
    bool hasHydrusTags() const { return !m_hydrusFlat.isEmpty(); }
    StrayTagEdit hydrusTagSplit() const;

private:
    void fetchFromHydrus();
    QPlainTextEdit *m_edit = nullptr;
    QLabel *m_status = nullptr;
    QPushButton *m_hydrusBtn = nullptr;
    Hydrus::Config m_hydrus;
    StrayTagEdit m_existing; // buckets already in the sidecar
    QString m_absFile;
    QStringList m_hydrusArtist;
    QStringList m_hydrusCharacter;
    QStringList m_hydrusCopyright;
    QStringList m_hydrusGeneral;
    QStringList m_hydrusMeta;
    QStringList m_hydrusFlat;
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
                                const Hydrus::Config &hydrus,
                                QWidget *parent = nullptr);
    QVector<StrayTagEdit> edits() const;

private:
    void commitCurrent();
    void loadRow(int row);
    void fetchHydrusRow();
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
    QPushButton *m_hydrusBtn = nullptr;
    Hydrus::Config m_hydrus;
};
