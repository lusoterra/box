#pragma once

#include "flowlayout.h"
#include "index.h"
#include "settings.h"

#include <QHash>
#include <QImage>
#include <QListView>
#include <QMainWindow>
#include <QPixmap>
#include <QProgressBar>
#include <QStandardItemModel>
#include <QStringList>
#include <QStyledItemDelegate>
#include <QVector>

class QComboBox;
class QCompleter;
class QDrag;
class QLabel;
class QLineEdit;
class QListWidget;
class QStackedWidget;
class QStringListModel;
class QSplitter;
class QTimer;
class TagTypeFetcher;
class ThumbLoader;
class VideoThumbGrabber;
class ViewerPage;
class WelcomePage;

// Grid delegate: uniform-aspect thumbnail + one info card per entry
// ([G] +12 #11984812) + filename line.
class GalleryDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    explicit GalleryDelegate(QObject *parent = nullptr);
    void setAspect(int num, int den);
    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option,
                   const QModelIndex &index) const override;

private:
    int m_num = 1;
    int m_den = 1;
};

// Grid with drag-out: drag thumbnails to other applications as file URLs.
class GridView : public QListView {
    Q_OBJECT
public:
    explicit GridView(QWidget *parent = nullptr);

protected:
    void startDrag(Qt::DropActions supportedActions) override;
};

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(const QString &root, bool rootExplicit = true,
                        QWidget *parent = nullptr);

protected:
    void closeEvent(QCloseEvent *e) override;

private slots:
    void openFolder();
    void openPath(const QString &path);
    void showWelcome();
    void openPreferences();
    void reload();
    void onClassifyProgress(int done, int total);
    void onClassifyFinished(const QHash<QString, QString> &buckets);
    void loadStylesheet();
    void applyFilter();
    void onSearchEdited(const QString &text);
    void onSearchReturn();
    void onCompleterActivated(const QString &text);
    void onRatingChanged();
    void onCategoryChanged();
    void onAspectChanged();
    void onTagClicked();
    void onGridActivated(const QModelIndex &index);
    void onThumbnailReady(const QString &key, const QImage &image);
    void onScanFinished();
    void onScanProgress(int n, int total);
    void onCacheStats(int hits, int misses);
    void requestVisibleThumbs();
    void backToGrid();

private:
    void startScan(const QString &root);
    void maybeClassify();
    void applyThemeFromSettings();
    void setAspect(int num, int den);
    void rebuildSidebar();
    void rebuildCombos();
    void rebuildTagTab(QListWidget *list, const QVector<QPair<QString, int>> &entries);
    void updateResultLabel();
    void showViewer(int row);

    QString m_root;
    Index m_index;
    QVector<Post> m_current; // last filtered result (full list, no pages)
    AppSettings m_settings;

    QStringList m_activeTags;
    QString m_searchQuery;
    QString m_rating = QStringLiteral("all");
    QString m_category;
    QString m_sort = QStringLiteral("id-desc");
    int m_aspectNum = 1;
    int m_aspectDen = 1;

    // UI
    QLineEdit *m_searchEdit = nullptr;
    QCompleter *m_completer = nullptr;
    QStringListModel *m_completeModel = nullptr;
    QTimer *m_completeTimer = nullptr;
    QTimer *m_thumbTimer = nullptr;
    QComboBox *m_ratingBox = nullptr;
    QComboBox *m_categoryBox = nullptr;
    QComboBox *m_sortBox = nullptr;
    QComboBox *m_aspectBox = nullptr;
    QLabel *m_countLabel = nullptr;
    QLabel *m_resultLabel = nullptr;
    QWidget *m_pillBar = nullptr;
    FlowLayout *m_pills = nullptr;
    QProgressBar *m_scanBar = nullptr;
    QListWidget *m_copyList = nullptr;
    QListWidget *m_charList = nullptr;
    QListWidget *m_artistList = nullptr;
    QListWidget *m_tagList = nullptr;
    QListWidget *m_metaList = nullptr;
    QStackedWidget *m_pages = nullptr;
    QStackedWidget *m_stage = nullptr; // 0 = welcome, 1 = browser
    WelcomePage *m_welcome = nullptr;
    QSplitter *m_sideSplit = nullptr; // resizable tag sections
    GridView *m_grid = nullptr;
    ViewerPage *m_viewer = nullptr;
    GalleryDelegate *m_delegate = nullptr;
    QStandardItemModel *m_model = nullptr;
    ThumbLoader *m_thumbs = nullptr;
    VideoThumbGrabber *m_videoThumbs = nullptr;
    TagTypeFetcher *m_fetcher = nullptr;
    QHash<QString, QPixmap> m_thumbCache; // request key -> pixmap
    QList<QString> m_cacheOrder;
    QPixmap m_placeholder;
    class ScanWatcher;
};
