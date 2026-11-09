#pragma once

#include "settings.h"

#include <QDialog>
#include <QLabel>
#include <QMap>
#include <QPlainTextEdit>

class QSpinBox;
class QDoubleSpinBox;

class QCheckBox;
class QComboBox;
class QLineEdit;
class QListWidget;
class QPushButton;
class QRadioButton;
class QSlider;

class FaviconCache;

// Preferences dialog editing a copy of AppSettings. On accept(), use
// settings() for the result; MainWindow applies + saves.
class PrefsDialog : public QDialog {
    Q_OBJECT
public:
    // favicons may be null (tests); when given, the favicon preview
    // falls back to the auto-fetched icon.
    explicit PrefsDialog(const AppSettings &current, FaviconCache *favicons = nullptr,
                         QWidget *parent = nullptr);
    AppSettings settings() const { return m_result; }

signals:
    // "Update tag categories now" (Library): run a forced classification
    // pass over the current archive, refetching even cached verdicts.
    void classifyNowRequested();
    // "Refresh favicons" (Browse): drop cached site icons and refetch.
    void faviconsRefreshRequested();
    // Apply button: harvest + let the owner apply live without closing.
    void applyRequested();

private:
    bool eventFilter(QObject *o, QEvent *e) override;
    void harvest();
    void harvestInto(AppSettings &out); // UI widgets over out (keeps history/session)
    void updateApplyState();            // grey Apply when nothing changed
    void setSwatch(const QColor &c);
    void setBooruSwatch(); // badge color button reflects m_booruColor
    void testSite(const BooruSite &probed);
    void detectBooruEngine(QPushButton *detectBtn);
    void showProbeDetails(); // full request/response behind the Test result
    void saveBooruEditor();   // commit editor fields into the current entry
    void commitBooruEditor(const QString &key); // commit into entry by key
    BooruSite *currentBooruEntry(); // entry behind the selected row, if any
    void updateCookieLabel();       // "N set"/"none" from the current entry
    void updateFaviconPreview();    // custom icon / staged choice / none
    QString customFaviconDir() const; // <dataDir>/favicons for overrides
    void commitPendingFavicons();   // staged icons -> files (Apply/OK only)
    void loadBooruEditor(int row); // fill editor from entry behind row
    void refreshBooruList();        // rebuild list from m_booruEdits
    void refreshBooruListLabels();  // update list texts in place

    AppSettings m_result;
    AppSettings m_initial; // last applied (or opened) state: drives Apply
    QString m_downloadDefault;
    QString m_dataDefault;
    QPushButton *m_applyBtn = nullptr;
    QComboBox *m_defaultTab;
    QComboBox *m_language = nullptr;
    QCheckBox *m_classify;
    QCheckBox *m_fullCache;
    QSpinBox *m_cacheLimit;
    QCheckBox *m_favAutoDownload;
    QCheckBox *m_favFav;
    QCheckBox *m_unfavDeletes;
    QCheckBox *m_deleteUnfavs;
    QPlainTextEdit *m_blacklist;
    QListWidget *m_favTagList = nullptr; // General: review/remove stars
    QLineEdit *m_configDir = nullptr; // General: effective config folder
    QLineEdit *m_defaultImageViewer = nullptr;
    QLineEdit *m_defaultGifViewer = nullptr;
    QLineEdit *m_defaultVideoPlayer = nullptr;
    QCheckBox *m_loopGif = nullptr;
    QCheckBox *m_loopVideo = nullptr;
    QComboBox *m_browseLimit;
    QListWidget *m_defBoorus = nullptr;
    QLineEdit *m_dataDir = nullptr;
    QLineEdit *m_cacheDir = nullptr;
    QCheckBox *m_cacheFavs = nullptr;
    QLineEdit *m_downloadDir;
    QComboBox *m_ugoiraFormat = nullptr;
    QSpinBox *m_ugoiraDelay = nullptr;
    QLineEdit *m_globalProxy = nullptr;
    QComboBox *m_dragMode = nullptr;
    QCheckBox *m_wallAutoPopup = nullptr;
    QListWidget *m_booruList = nullptr;
    QLineEdit *m_booruName = nullptr;
    QLineEdit *m_booruBase = nullptr;
    QComboBox *m_booruEngine = nullptr;
    QLineEdit *m_booruUser = nullptr;
    QLineEdit *m_booruSecret = nullptr;
    QLabel *m_booruCookieLabel = nullptr;
    QLabel *m_booruFavicon = nullptr;
    // Staged custom favicons (key -> source image; "" = remove on Apply).
    // Files are only touched on Apply: Cancel leaves everything alone.
    QMap<QString, QString> m_pendingFavicons;
    QLineEdit *m_booruShort = nullptr;
    QPushButton *m_booruColorBtn = nullptr;
    QColor m_booruColor;
    QCheckBox *m_booruNoClassify = nullptr;
    QComboBox *m_booruDragMode = nullptr;
    QCheckBox *m_booruLocalBlacklist = nullptr;
    QRadioButton *m_booruCacheInherit = nullptr;
    QRadioButton *m_booruCacheAll = nullptr;
    QRadioButton *m_booruCacheSfw = nullptr;
    QRadioButton *m_booruCacheNone = nullptr;
    QCheckBox *m_booruEnabled = nullptr;
    QCheckBox *m_sidebarSingleScroll = nullptr;
    QCheckBox *m_gridSnap = nullptr;
    QCheckBox *m_gridStaggered = nullptr;
    QCheckBox *m_plusLocalFolder = nullptr;
    QLineEdit *m_booruTags = nullptr;
    QLineEdit *m_booruProxy = nullptr;
    QSlider *m_mascotOpacity = nullptr;
    QLabel *m_mascotOpacityLabel = nullptr;
    QSpinBox *m_mascotMargin = nullptr;
    QWidget *m_booruFormBox = nullptr;    QLabel *m_booruEmpty = nullptr;
    QList<BooruSite> m_booruEdits;
    QLabel *m_accStatus;
    FaviconCache *m_favicons = nullptr; // auto icons for the preview box
    RemoteSource::CredentialProbe m_lastProbe;
    bool m_lastProbeValid = false;
    // Reentrancy guard: programmatic field fills in loadBooruEditor emit
    // textChanged, which would otherwise commit half-loaded widgets back
    // into the entry being loaded (base/type/creds clobbered on every
    // row switch). While set, commitBooruEditor is a no-op.
    bool m_loadingBooru = false;
    QCheckBox *m_indexCache;
    QRadioButton *m_cacheAll = nullptr;
    QRadioButton *m_cacheSfw = nullptr;
    QRadioButton *m_cacheNone = nullptr;
    QPushButton *m_deleteMediaCache = nullptr;
    QListWidget *m_tagOverrides = nullptr;
    QLineEdit *m_tagOverrideEdit = nullptr;
    QListWidget *m_customBuckets = nullptr; // native category -> bucket
    QLineEdit *m_customBucketEdit = nullptr;
    QComboBox *m_customBucketBucket = nullptr;
    QListWidget *m_customCategories = nullptr; // user-created groups
    QLineEdit *m_customCategoryEdit = nullptr;
    QListWidget *m_tagCustomAssignments = nullptr; // tag -> custom group
    QLineEdit *m_tagCustomEdit = nullptr;
    QComboBox *m_tagCustomCategory = nullptr;
    QComboBox *m_tagOverrideBucket = nullptr;
    QListWidget *m_categoryMerges = nullptr;
    QComboBox *m_mergeScope = nullptr;
    QLineEdit *m_mergeSources = nullptr;
    QLineEdit *m_mergeTarget = nullptr;
    QComboBox *m_thumbSize;
    QComboBox *m_themeMode;
    QPushButton *m_accentBtn;
    QColor m_accent;
    QLineEdit *m_stylesheet;
    QLineEdit *m_mascot = nullptr;
    bool m_mascotHidden = false;
    QComboBox *m_aspect;
    QDoubleSpinBox *m_scrollRows = nullptr;
    int m_customNum = 1;
    int m_customDen = 1;
    QPlainTextEdit *m_explicit;
    QLabel *m_explicitCount;
    QCheckBox *m_muted;
    QCheckBox *m_loop;
    QSlider *m_volume;
    QComboBox *m_maxSide;
};
