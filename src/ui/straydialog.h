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
