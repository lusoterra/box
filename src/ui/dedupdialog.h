#pragma once

#include "dedupdb.h"
#include "dedupscanner.h"

#include <QDialog>
#include <QFutureWatcher>
#include <QHash>
#include <QPixmap>
#include <QString>
#include <QStringList>

class QCheckBox;
class QCloseEvent;
class QGroupBox;
class QLabel;
class QListWidget;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QTreeWidget;
class QTreeWidgetItem;
class DedupScanTask;
namespace HydrusStore {
class Db;
}

struct DedupDialogSources {
    QString dbPath;
    QString downloadDir;
    QString hydrusDir;
    QString hydrusDbPath;
    QStringList archiveRoots;
    QString currentRoot;
    bool allowArchiveDelete = false;
};

// Reviews pixel-hash duplicate groups and deletes the copies the user
// confirms. Deletion is limited to:
//   - the configured download folder,
//   - the Hydrus media folder (and its hydrus.db row),
//   - archive roots, only with the explicit opt-in setting.
// Media sidecars (`<stem>.json`, `<media>.json`) are removed with the
// media. The dialog is window-modal; scanning runs in a worker.
class DedupDialog : public QDialog {
    Q_OBJECT
public:
    DedupDialog(const DedupDialogSources &sources, HydrusStore::Db *hydrus,
                QWidget *parent = nullptr);
    ~DedupDialog() override;

signals:
    // Successful deletions: archive roots let the shell invalidate local
    // tabs; download/hydrus refreshes are handled by the shell too.
    void filesDeleted(const QStringList &paths,
                      const QStringList &archiveRoots);

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    void startScan();
    void scanFinished();
    void keepSelected();
    void deleteSelected();
    void clearDatabase();
    void updateDeleteButton();
    void showDetails();
    void addArchiveFolder();

private:
    struct Meta {
        int tags = 0;
        QString text;
    };

    void buildUi();
    void reloadGroups();
    void setScanning(bool scanning);
    QVector<DedupSource> selectedSources() const;
    QStringList allArchiveRoots() const;
    Meta metaFor(const DedupFile &file);
    QPixmap thumbFor(const DedupFile &file);
    QString scopeForItem(const QTreeWidgetItem *item) const;
    QString pathForItem(const QTreeWidgetItem *item) const;
    bool deletable(const DedupFile &file) const;
    bool pathAllowed(const DedupFile &file) const;
    static QString formatBytes(qint64 bytes);
    static bool isUnder(const QString &path, const QString &dir);

    DedupDialogSources m_sources;
    HydrusStore::Db *m_hydrus = nullptr;
    DedupDb m_db;
    QHash<QString, DedupFile> m_rows; // scope + '\n' + path
    QHash<QString, Meta> m_meta;
    QHash<QString, QPixmap> m_thumbs;
    bool m_scanning = false;
    bool m_closeWhenDone = false;
    DedupScanTask *m_task = nullptr;
    QFutureWatcher<void> *m_watcher = nullptr;

    QGroupBox *m_sourcesBox = nullptr;
    QCheckBox *m_downloadsBox = nullptr;
    QCheckBox *m_hydrusBox = nullptr;
    QListWidget *m_archives = nullptr;
    QSpinBox *m_threshold = nullptr;
    QProgressBar *m_progress = nullptr;
    QLabel *m_status = nullptr;
    QPushButton *m_scanBtn = nullptr;
    QPushButton *m_stopBtn = nullptr;
    QPushButton *m_keepBtn = nullptr;
    QPushButton *m_deleteBtn = nullptr;
    QPushButton *m_clearBtn = nullptr;
    QTreeWidget *m_tree = nullptr;
    QPlainTextEdit *m_details = nullptr;
};
