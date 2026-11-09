#include "prefsdlg.h"
#include "cachejanitor.h"
#include "cachepolicy.h"
#include "cookiegrab.h"
#include "faviconcache.h"
#include "flowlayout.h"
#include "tagtypes.h"
#include "theme.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QEventLoop>
#include <QFileDialog>
#include <QFontDatabase>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTimer>
#include <QScrollArea>
#include <QStandardItem>
#include <QStandardItemModel>
#include <QSlider>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTabWidget>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent>

#include <utility>

#include "remotesource.h"

namespace {
// Scrolling the (scrollable) dialog must not flip controls under the
// cursor: wheel only reaches a control after it was clicked.
//
// - Closed dropdowns never change: click to open, then the popup list
//   (its own view, unguarded) scrolls normally.
// - Spins/sliders/lists/multi-line edits need keyboard focus (a click)
//   first; otherwise the gesture scrolls the settings page instead.
// Wheel events often target inner children (combo/spin line edits) or
// viewports rather than the control itself, so those are guarded too and
// mapped back to their owning control.
class WheelGuard : public QObject {
public:
    explicit WheelGuard(QObject *parent = nullptr) : QObject(parent) {}
    bool eventFilter(QObject *o, QEvent *e) override {
        if (e->type() != QEvent::Wheel)
            return false;
        QWidget *w = qobject_cast<QWidget *>(o);
        if (!w)
            return false;
        // Map viewports / inner children back to the owning control.
        QWidget *owner = w;
        auto isControl = [](QWidget *x) {
            return qobject_cast<QComboBox *>(x) ||
                   qobject_cast<QAbstractSpinBox *>(x) ||
                   qobject_cast<QSlider *>(x) ||
                   qobject_cast<QAbstractItemView *>(x) ||
                   qobject_cast<QPlainTextEdit *>(x);
        };
        if (!isControl(owner)) {
            owner = w->parentWidget();
            while (owner && !isControl(owner))
                owner = owner->parentWidget();
            if (!owner)
                return false; // not one of ours: leave alone
        }
        bool allow = false;
        if (QComboBox *cb = qobject_cast<QComboBox *>(owner)) {
            allow = cb->view() && cb->view()->isVisible(); // popup open
        } else if (qobject_cast<QAbstractSpinBox *>(owner) ||
                   qobject_cast<QSlider *>(owner)) {
            // Number inputs never take the wheel here: focus lingers
            // after typing/dragging, so every later hover-wheel would
            // silently rewrite the value. Type, nudge with the arrows,
            // or drag - the page scrolls instead.
            allow = false;
        } else {
            allow = owner->hasFocus(); // clicked first
        }
        if (allow)
            return false;
        // Block the control but forward the gesture to the enclosing
        // settings page so scrolling still works under the cursor.
        e->ignore();
        QWidget *page = w->parentWidget();
        while (page && !qobject_cast<QScrollArea *>(page))
            page = page->parentWidget();
        if (page) {
            if (QScrollArea *sa = qobject_cast<QScrollArea *>(page))
                QCoreApplication::sendEvent(sa->viewport(), e);
        }
        return true;
    }
};
} // namespace

PrefsDialog::PrefsDialog(const AppSettings &current, FaviconCache *favicons,
                         QWidget *parent)
    : QDialog(parent), m_result(current), m_initial(current),
      m_favicons(favicons), m_accent(current.accent) {
    setWindowTitle(tr("Preferences"));
    setModal(true);
    // Wide enough that the Booru manager editor (name/badge/base/type +
    // cookie buttons) fits at 1600x900 and 1080p without overflow; the
    // page scrolls vertically, never horizontally.
    resize(980, 820);
    auto *outer = new QVBoxLayout(this);
    auto *tabs = new QTabWidget(this);
    // Document mode: no tab-pane frame. The framed pane rounds its
    // bottom corners open under Fusion (a border that visibly doesn't
    // close); the flat document look sidesteps it in every theme.
    tabs->setDocumentMode(true);
    outer->addWidget(tabs, 1);
    auto makePage = [this, tabs](const QString &title) {
        auto *scroll = new QScrollArea(this);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        // A settings page must never grow a horizontal scrollbar: fields
        // shrink/wrap instead (long booru rows were widening the dialog).
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scroll->setSizeAdjustPolicy(QAbstractScrollArea::AdjustIgnored);
        // One flat surface: viewport + page share the window background
        // so no inner box edge shows through around the content.
        scroll->setBackgroundRole(QPalette::Window);
        scroll->viewport()->setBackgroundRole(QPalette::Window);
        scroll->viewport()->setAutoFillBackground(false);
        auto *page = new QWidget(this);
        page->setBackgroundRole(QPalette::Window);
        page->setMinimumWidth(0);
        page->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        auto *l = new QVBoxLayout(page);
        l->setContentsMargins(0, 0, 8, 8);
        l->setSpacing(8);
        l->setSizeConstraint(QLayout::SetNoConstraint);
        scroll->setWidget(page);
        tabs->addTab(scroll, title);
        return l;
    };
    // General tab keeps the `lay` name so the sections below read on.
    QVBoxLayout *lay = makePage(tr("General"));
    QVBoxLayout *libraryPage = makePage(tr("Library"));
    QVBoxLayout *browsePage = makePage(tr("Browse"));
    QVBoxLayout *tagsPage = makePage(tr("Taxonomies"));
    QVBoxLayout *lookPage = makePage(tr("Appearance"));

    auto note = [this](const QString &text) {
        auto *l = new QLabel(text, this);
        l->setWordWrap(true);
        return l;
    };

    // ── General ──
    auto *gen = new QGroupBox(tr("General"), this);
    auto *genLay = new QVBoxLayout(gen);
    auto *defRow = new QHBoxLayout();
    defRow->addWidget(new QLabel(tr("Default tab:"), this));
    m_defaultTab = new QComboBox(this);
    m_defaultTab->addItem(tr("Welcome home"), QStringLiteral("welcome"));
    m_defaultTab->addItem(tr("Last session"), QStringLiteral("last"));
    // Default booru (driven by Browse > Default boorus): open the first
    // configured default booru as a browse tab instead of a per-site list.
    const QList<BooruSite> defBoorus =
        current.boorus.isEmpty() ? RemoteSource::presetBoorus() : current.boorus;
    QStringList defKeys;
    for (const BooruSite &s : defBoorus)
        if (s.enabled && current.browseSites.contains(s.key))
            defKeys << s.key;
    if (defKeys.isEmpty()) {
        for (const BooruSite &s : defBoorus)
            if (s.enabled && !s.key.isEmpty()) {
                defKeys << s.key;
                break;
            }
    }
    if (!defKeys.isEmpty()) {
        const BooruSite *pick = nullptr;
        for (const BooruSite &s : defBoorus)
            if (s.key == defKeys.first()) { pick = &s; break; }
        const QString label = pick ? pick->label : defKeys.first();
        m_defaultTab->addItem(tr("Default booru (%1)").arg(label),
                              defKeys.first());
    }
    int defaultIdx = m_defaultTab->findData(current.tabsDefault);
    if (defaultIdx < 0 && !current.tabsDefault.isEmpty() &&
        current.tabsDefault != QStringLiteral("welcome") &&
        current.tabsDefault != QStringLiteral("last") &&
        current.tabsDefault != defKeys.value(0)) {
        m_defaultTab->addItem(
            tr("%1 (disabled)").arg(current.tabsDefault),
            current.tabsDefault);
        defaultIdx = m_defaultTab->count() - 1;
    }
    m_defaultTab->setCurrentIndex(qMax(0, defaultIdx));
    defRow->addWidget(m_defaultTab, 1);
    genLay->addLayout(defRow);
    auto *langRow = new QHBoxLayout();
    langRow->addWidget(new QLabel(tr("Language:"), this));
    m_language = new QComboBox(this);
    m_language->setToolTip(tr("Applies after a restart"));
    m_language->addItem(tr("System default"), QString());
    m_language->addItem(QStringLiteral("English (UK)"), QStringLiteral("en_GB"));
    m_language->addItem(QStringLiteral("Français"), QStringLiteral("fr"));
    m_language->addItem(QStringLiteral("Português (Brasil)"), QStringLiteral("pt_BR"));
    m_language->setCurrentIndex(
        qMax(0, m_language->findData(current.language)));
    langRow->addWidget(m_language, 1);
    genLay->addLayout(langRow);
    genLay->addWidget(note(tr("Language applies on next launch.")));
    // Config folder (where settings.ini lives). Moving copies the current
    // files to the chosen folder and leaves a pointer stub at the default
    // location; the app restarts from the stub on next launch.
    auto *cfgRow = new QHBoxLayout();
    cfgRow->addWidget(new QLabel(tr("Config folder:"), this));
    m_configDir = new QLineEdit(effectiveConfigDir(), this);
    m_configDir->setReadOnly(true);
    m_configDir->setToolTip(tr("Effective config folder (settings.ini lives here)"));
    cfgRow->addWidget(m_configDir, 1);
    auto *cfgMove = new QPushButton(tr("Move…"), this);
    cfgMove->setToolTip(tr("Move the config files to a chosen folder"));
    connect(cfgMove, &QPushButton::clicked, this, [this] {
        const QString dest = QFileDialog::getExistingDirectory(
            this, tr("Move config folder to"), m_configDir->text());
        if (dest.isEmpty())
            return;
        const QString cleanDest =
            QDir(dest).absolutePath();
        const QString fromDir = effectiveConfigDir();
        const QString defaultDir =
            QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) +
            QStringLiteral("/box");
        if (cleanDest == fromDir)
            return;
        // Flush pending edits to the old location first, so nothing is lost.
        harvest();
        saveSettings(m_result);
        if (QMessageBox::question(
                this, tr("Move config folder"),
                tr("Move %1 to %2? settings.ini and boorus.json are copied "
                   "there and the default folder keeps a pointer stub.")
                    .arg(fromDir, cleanDest),
                QMessageBox::Yes | QMessageBox::No,
                QMessageBox::No) != QMessageBox::Yes)
            return;
        const QString moveErr = moveConfigFiles(fromDir, cleanDest);
        if (!moveErr.isEmpty()) {
            QMessageBox::warning(this, tr("Move config folder"), moveErr);
            return;
        }
        setConfigDirOverride(cleanDest);
        m_configDir->setText(effectiveConfigDir());
        if (cleanDest != defaultDir) {
            const QString stubErr = writeConfigStub(defaultDir, cleanDest);
            if (!stubErr.isEmpty()) {
                QMessageBox::warning(this, tr("Move config folder"), stubErr);
                return;
            }
        }
        QMessageBox::information(
            this, tr("Move config folder"),
            tr("Config moved. The app will use the new folder from the "
               "next launch."));
        updateApplyState();
    });
    cfgRow->addWidget(cfgMove);
    genLay->addLayout(cfgRow);
    auto *histRow = new QHBoxLayout();
    histRow->addWidget(new QLabel(tr("Archive history:"), this));
    histRow->addStretch(1);
    auto *histClear = new QPushButton(tr("Clear history"), this);
    connect(histClear, &QPushButton::clicked, this, [this] {
        m_result.history.clear();
        // Also drop the saved session: "last session" startup must have
        // nothing stale to resume (this is what actually stuck around).
        m_result.lastRoot.clear();
        m_result.savedTabs.clear();
        m_result.savedTabCurrent = 0;
        QMessageBox::information(nullptr, tr("History"),
                                 tr("Archive history cleared."));
        updateApplyState();
    });
    histRow->addWidget(histClear);
    genLay->addLayout(histRow);
    // Favourite tags (starred via right-click anywhere): listed here for
    // review and removal. Starring still happens out in the app; this is
    // the only place that can bulk-remove.
    genLay->addWidget(new QLabel(tr("Favourite tags:"), this));
    m_favTagList = new QListWidget(this);
    m_favTagList->setSelectionMode(QAbstractItemView::ExtendedSelection);
    for (const QString &t : current.favTags) {
        if (!t.trimmed().isEmpty())
            m_favTagList->addItem(t);
    }
    m_favTagList->setMinimumHeight(90);
    genLay->addWidget(m_favTagList);
    auto *favTagRow = new QHBoxLayout();
    favTagRow->addStretch(1);
    auto *favTagRemove = new QPushButton(tr("Remove selected"), this);
    favTagRemove->setToolTip(tr("Unstar the selected tags"));
    connect(favTagRemove, &QPushButton::clicked, this, [this] {
        for (QListWidgetItem *it : m_favTagList->selectedItems())
            delete m_favTagList->takeItem(m_favTagList->row(it));
        updateApplyState();
    });
    favTagRow->addWidget(favTagRemove);
    genLay->addLayout(favTagRow);
    lay->addWidget(gen);

    // ── Favourites ──
    auto *fav = new QGroupBox(tr("Favourites"), this);
    auto *favLay = new QVBoxLayout(fav);
    m_favAutoDownload = new QCheckBox(
        tr("Auto-download what I favourite (full quality)"), this);
    m_favAutoDownload->setChecked(current.favAutoDownload);
    m_favFav = new QCheckBox(
        tr("Auto-favourite what I download"), this);
    m_favFav->setChecked(current.favAutoFav);
    m_unfavDeletes = new QCheckBox(
        tr("Unfavouriting deletes the downloaded file"), this);
    m_unfavDeletes->setChecked(current.unfavDeletesFile);
    m_deleteUnfavs = new QCheckBox(
        tr("Deleting a file also removes it from favourites"), this);
    m_deleteUnfavs->setChecked(current.deleteUnfavs);
    favLay->addWidget(m_favAutoDownload);
    favLay->addWidget(m_favFav);
    favLay->addWidget(m_unfavDeletes);
    favLay->addWidget(m_deleteUnfavs);
    favLay->addWidget(note(QStringLiteral(
        "Favourites are database entries only - no files are written unless "
        "auto-download is on.")));
    lay->addWidget(fav);

    // ── Filtering ──
    auto *filter = new QGroupBox(tr("Filtering"), this);
    auto *filterLay = new QVBoxLayout(filter);
    filterLay->addWidget(note(QStringLiteral(
        "Blacklisted tags are hidden everywhere (local tabs, browse pages, "
        "favourites). One per line.")));
    m_blacklist = new QPlainTextEdit(this);
    m_blacklist->setPlainText(current.blacklist.join(u'\n'));
    m_blacklist->setTabChangesFocus(true);
    m_blacklist->setMinimumHeight(90);
    filterLay->addWidget(m_blacklist);
    lay->addWidget(filter);

    // ── Library / cache ──
    auto *lib = new QGroupBox(tr("Library cache"), this);
    auto *libLay = new QVBoxLayout(lib);
    m_indexCache = new QCheckBox(
        tr("Scan index cache (fast rescans of large archives)"), this);
    m_indexCache->setChecked(current.indexCache);
    libLay->addWidget(m_indexCache);
    libLay->addWidget(new QLabel(tr("Media cache:"), this));
    m_cacheAll = new QRadioButton(tr("Cache everything"), lib);
    m_cacheSfw = new QRadioButton(
        tr("Cache only SFW pictures/videos"), lib);
    m_cacheNone = new QRadioButton(
        tr("Cache nothing (session only)"), lib);
    auto *cacheGroup = new QButtonGroup(this);
    for (QRadioButton *r : {m_cacheAll, m_cacheSfw, m_cacheNone})
        cacheGroup->addButton(r);
    switch (current.cacheMode) {
    case CacheMode::None:
        m_cacheNone->setChecked(true);
        break;
    case CacheMode::SfwOnly:
        m_cacheSfw->setChecked(true);
        break;
    case CacheMode::All:
    default:
        m_cacheAll->setChecked(true);
        break;
    }
    libLay->addWidget(m_cacheAll);
    libLay->addWidget(m_cacheSfw);
    libLay->addWidget(m_cacheNone);
    libLay->addWidget(note(tr(
        "Media cache = thumbnails and full-quality files kept on disk. "
        "\"Nothing\" keeps media in memory/session temp only; downloads you "
        "explicitly request always land in the download folder. The "
        "favourites database and scan index stay.")));
    auto selectedMode = [this] {
        if (m_cacheNone->isChecked())
            return CacheMode::None;
        if (m_cacheSfw->isChecked())
            return CacheMode::SfwOnly;
        return CacheMode::All;
    };
    m_classify = new QCheckBox(
        tr("Classify untyped tags online (Gelbooru/Danbooru, cached)"),
        this);
    m_classify->setChecked(current.classifyTags);
    auto *classifyRow = new QHBoxLayout();
    auto *classifyNow = new QPushButton(tr("Update tag categories now"), this);
    classifyNow->setToolTip(tr("Re-fetch categories for every untyped tag, "
                               "including cached ones"));
    connect(classifyNow, &QPushButton::clicked, this,
            &PrefsDialog::classifyNowRequested);
    classifyRow->addWidget(classifyNow);
    classifyRow->addStretch(1);
    m_fullCache = new QCheckBox(
        tr("Also cache full-quality images while browsing (large)"), this);
    m_fullCache->setChecked(current.fullCache);
    m_cacheFavs = new QCheckBox(
        tr("Cache media opened from Favourites too"), this);
    m_cacheFavs->setChecked(current.cacheFavs);
    auto *limitRow = new QHBoxLayout();
    limitRow->addWidget(new QLabel(tr("Cache size limit:"), this));
    m_cacheLimit = new QSpinBox(this);
    m_cacheLimit->setRange(0, 1048576);
    m_cacheLimit->setSingleStep(256);
    m_cacheLimit->setSuffix(tr(" MB (0 = unlimited)"));
    m_cacheLimit->setValue(current.cacheLimitMB);
    limitRow->addWidget(m_cacheLimit, 1);
    // Cache folder override (empty = default ~/.cache/box).
    m_cacheDir = new QLineEdit(current.cacheDir, this);
    auto *cacheBrowse = new QPushButton(tr("Browse…"), this);
    connect(cacheBrowse, &QPushButton::clicked, this, [this] {
        const QString d = QFileDialog::getExistingDirectory(
            this, tr("Cache folder"), m_cacheDir->text());
        if (!d.isEmpty())
            m_cacheDir->setText(d);
    });
    auto *cacheRow = new QHBoxLayout();
    cacheRow->addWidget(m_cacheDir, 1);
    cacheRow->addWidget(cacheBrowse);
    // (added to libLay below alongside limitRow)
    auto *sizeRow = new QHBoxLayout();
    sizeRow->addWidget(new QLabel(tr("Thumbnail size:"), this));
    m_thumbSize = new QComboBox(this);
    for (int s : {256, 360, 512, 768})
        m_thumbSize->addItem(tr("%1 px").arg(s), s);
    m_thumbSize->setCurrentIndex(
        qMax(0, m_thumbSize->findData(current.thumbLongSide)));
    sizeRow->addWidget(m_thumbSize);
    sizeRow->addStretch(1);
    m_deleteMediaCache = new QPushButton(tr("Delete cached media…"), this);
    auto syncDelete = [this] {
        const bool on = !m_cacheAll->isChecked();
        m_deleteMediaCache->setEnabled(on);
        m_deleteMediaCache->setToolTip(
            on ? (m_cacheNone->isChecked()
                      ? tr("Remove every cached thumbnail and full image")
                      : tr("Remove cached NSFW media, leaving SFW entries"))
               : tr("Disabled while caching everything"));
    };
    connect(m_cacheAll, &QRadioButton::toggled, this,
            [syncDelete] { syncDelete(); });
    connect(m_cacheSfw, &QRadioButton::toggled, this,
            [syncDelete] { syncDelete(); });
    connect(m_cacheNone, &QRadioButton::toggled, this,
            [syncDelete] { syncDelete(); });
    syncDelete();
    connect(m_deleteMediaCache, &QPushButton::clicked, this,
            [this, selectedMode] {
                const CacheMode mode = selectedMode();
                if (mode == CacheMode::All)
                    return;
                const bool everything = mode == CacheMode::None;
                const QString question =
                    everything
                        ? tr("Delete every cached thumbnail and full image?\n\n"
                             "Downloads, favourites and the scan index are "
                             "not touched.")
                        : tr("Delete cached NSFW media?\n\n"
                             "SFW cached entries stay; NSFW and unknown/legacy "
                             "entries are removed. Downloads and favourites "
                             "are not touched.");
                if (QMessageBox::question(this, tr("Delete cached media"),
                                          question) != QMessageBox::Yes)
                    return;
                const int removed =
                    CacheJanitor::purgeMedia(/*keepSfwOnly=*/!everything);
                QMessageBox::information(
                    this, tr("Delete cached media"),
                    tr("Removed %1 cached file(s).").arg(removed));
            });
    auto *clearBtn = new QPushButton(tr("Clear scan index…"), this);
    connect(clearBtn, &QPushButton::clicked, this, [] {
        clearIndexCache();
        QMessageBox::information(
            nullptr, tr("Scan index"),
            tr("Scan index cleared. It rebuilds on the next scan; "
               "cached media is untouched."));
    });
    sizeRow->addWidget(m_deleteMediaCache);
    sizeRow->addWidget(clearBtn);
    libLay->addWidget(m_fullCache);
    libLay->addWidget(m_cacheFavs);
    libLay->addWidget(m_classify);
    libLay->addLayout(classifyRow);
    libLay->addLayout(limitRow);
    auto *cacheLabelRow = new QHBoxLayout();
    cacheLabelRow->addWidget(new QLabel(tr("Cache folder:"), this));
    cacheLabelRow->addLayout(cacheRow);
    libLay->addLayout(cacheLabelRow);
    libLay->addWidget(note(tr("Untyped tags (tagType \"none\", Gelbooru blobs) get their category "
        "from public tag APIs. Results cache forever; misses stay general.")));
    libLay->addLayout(sizeRow);
    libLay->addWidget(note(tr("Cache changes apply on Apply; existing files "
                              "stay until you delete them.")));
    libraryPage->addWidget(lib);

    // ── Tag type overrides: manual bucket per tag ──
    auto *types = new QGroupBox(tr("Tag type overrides"), this);
    auto *typesLay = new QVBoxLayout(types);
    typesLay->addWidget(note(tr(
        "Tags the online classifier missed or got wrong (e.g. lucky_star → "
        "Copyright). Overrides apply on every scan without any network use "
        "and win over fetched categories.")));
    typesLay->addWidget(note(tr(
        "Native custom categories (Nuuru flags, Szurubooru categories) "
        "can be folded into a classic bucket below; unmapped ones keep "
        "their own sidebar section in browse tabs.")));
    m_tagOverrides = new QListWidget(this);
    m_tagOverrides->setMinimumHeight(90);
    for (auto it = current.manualTagTypes.begin();
         it != current.manualTagTypes.end(); ++it) {
        auto *item = new QListWidgetItem(
            tr("%1 → %2").arg(it.key(), it.value()), m_tagOverrides);
        item->setData(Qt::UserRole, it.key());
        item->setData(Qt::UserRole + 1, it.value());
    }
    typesLay->addWidget(m_tagOverrides);
    auto *typeRow = new QHBoxLayout();
    m_tagOverrideEdit = new QLineEdit(this);
    m_tagOverrideEdit->setPlaceholderText(tr("tag"));
    m_tagOverrideBucket = new QComboBox(this);
    m_tagOverrideBucket->addItem(tr("Artist"), QStringLiteral("artist"));
    m_tagOverrideBucket->addItem(tr("Copyright"), QStringLiteral("copyright"));
    m_tagOverrideBucket->addItem(tr("Character"), QStringLiteral("character"));
    m_tagOverrideBucket->addItem(tr("Meta"), QStringLiteral("meta"));
    m_tagOverrideBucket->addItem(tr("General"), QStringLiteral("general"));
    auto *typeAdd = new QPushButton(tr("Add"), this);
    auto *typeDel = new QPushButton(tr("Remove"), this);
    connect(typeAdd, &QPushButton::clicked, this, [this] {
        const QString tag = m_tagOverrideEdit->text().trimmed().toLower();
        if (tag.isEmpty())
            return;
        const QString bucket = m_tagOverrideBucket->currentData().toString();
        for (int i = 0; i < m_tagOverrides->count(); ++i) {
            QListWidgetItem *item = m_tagOverrides->item(i);
            if (item->data(Qt::UserRole).toString() == tag) {
                item->setText(tr("%1 → %2").arg(tag, bucket));
                item->setData(Qt::UserRole + 1, bucket);
                m_tagOverrideEdit->clear();
                updateApplyState();
                return;
            }
        }
        auto *item = new QListWidgetItem(tr("%1 → %2").arg(tag, bucket),
                                         m_tagOverrides);
        item->setData(Qt::UserRole, tag);
        item->setData(Qt::UserRole + 1, bucket);
        m_tagOverrideEdit->clear();
        updateApplyState();
    });
    connect(typeDel, &QPushButton::clicked, this, [this] {
        delete m_tagOverrides->takeItem(m_tagOverrides->currentRow());
        updateApplyState();
    });
    typeRow->addWidget(m_tagOverrideEdit, 1);
    typeRow->addWidget(m_tagOverrideBucket);
    typeRow->addWidget(typeAdd);
    typeRow->addWidget(typeDel);
    typesLay->addLayout(typeRow);
    typesLay->addWidget(note(tr(
        "Custom category → classic bucket (e.g. series → Copyright): "
        "those tags join that sidebar section and pill colour.")));
    m_customBuckets = new QListWidget(this);
    m_customBuckets->setMinimumHeight(70);
    for (auto it = current.customBucketMap.begin();
         it != current.customBucketMap.end(); ++it) {
        auto *item = new QListWidgetItem(
            tr("%1 → %2").arg(it.key(), it.value()), m_customBuckets);
        item->setData(Qt::UserRole, it.key());
        item->setData(Qt::UserRole + 1, it.value());
    }
    typesLay->addWidget(m_customBuckets);
    auto *customRow = new QHBoxLayout();
    m_customBucketEdit = new QLineEdit(this);
    m_customBucketEdit->setPlaceholderText(tr("category (e.g. series)"));
    m_customBucketBucket = new QComboBox(this);
    for (const auto &[label, key] :
         QList<QPair<QString, QString>>{{tr("Artist"), QStringLiteral("artist")},
                                        {tr("Copyright"), QStringLiteral("copyright")},
                                        {tr("Character"), QStringLiteral("character")},
                                        {tr("Meta"), QStringLiteral("meta")},
                                        {tr("General"), QStringLiteral("general")}}) {
        m_customBucketBucket->addItem(label, key);
    }
    auto *customAdd = new QPushButton(tr("Add"), this);
    auto *customDel = new QPushButton(tr("Remove"), this);
    connect(customAdd, &QPushButton::clicked, this, [this] {
        const QString cat =
            m_customBucketEdit->text().trimmed().toLower();
        if (cat.isEmpty())
            return;
        const QString bucket = m_customBucketBucket->currentData().toString();
        for (int i = 0; i < m_customBuckets->count(); ++i) {
            QListWidgetItem *item = m_customBuckets->item(i);
            if (item->data(Qt::UserRole).toString() == cat) {
                item->setText(tr("%1 → %2").arg(cat, bucket));
                item->setData(Qt::UserRole + 1, bucket);
                m_customBucketEdit->clear();
                updateApplyState();
                return;
            }
        }
        auto *item = new QListWidgetItem(tr("%1 → %2").arg(cat, bucket),
                                         m_customBuckets);
        item->setData(Qt::UserRole, cat);
        item->setData(Qt::UserRole + 1, bucket);
        m_customBucketEdit->clear();
        updateApplyState();
    });
    connect(customDel, &QPushButton::clicked, this, [this] {
        delete m_customBuckets->takeItem(m_customBuckets->currentRow());
        updateApplyState();
    });
    customRow->addWidget(m_customBucketEdit, 1);
    customRow->addWidget(m_customBucketBucket);
    customRow->addWidget(customAdd);
    customRow->addWidget(customDel);
    typesLay->addLayout(customRow);

    // ── User-created categories: assign any tag to its own group ──
    typesLay->addWidget(note(tr(
        "Custom tag categories: create a group, then assign tags to it. "
        "Assigned tags move out of Artist/Character/Copyright/Meta/Tags "
        "into that group in the sidebar and viewer.")));
    m_customCategories = new QListWidget(this);
    m_customCategories->setMinimumHeight(58);
    for (const QString &cat : current.customTagCategories)
        new QListWidgetItem(cat, m_customCategories);
    typesLay->addWidget(m_customCategories);
    auto *customCatRow = new QHBoxLayout();
    m_customCategoryEdit = new QLineEdit(this);
    m_customCategoryEdit->setPlaceholderText(tr("new category name"));
    auto *customCatAdd = new QPushButton(tr("Add category"), this);
    auto *customCatDel = new QPushButton(tr("Remove category"), this);
    customCatRow->addWidget(m_customCategoryEdit, 1);
    customCatRow->addWidget(customCatAdd);
    customCatRow->addWidget(customCatDel);
    typesLay->addLayout(customCatRow);

    m_tagCustomAssignments = new QListWidget(this);
    m_tagCustomAssignments->setMinimumHeight(80);
    for (auto it = current.tagCustomCategories.begin();
         it != current.tagCustomCategories.end(); ++it) {
        auto *item = new QListWidgetItem(
            tr("%1 → %2").arg(it.key(), it.value()), m_tagCustomAssignments);
        item->setData(Qt::UserRole, it.key());
        item->setData(Qt::UserRole + 1, it.value());
    }
    typesLay->addWidget(m_tagCustomAssignments);
    auto *catTagRow = new QHBoxLayout();
    m_tagCustomEdit = new QLineEdit(this);
    m_tagCustomEdit->setPlaceholderText(tr("tag"));
    m_tagCustomCategory = new QComboBox(this);
    m_tagCustomCategory->setEditable(true);
    m_tagCustomCategory->setPlaceholderText(tr("category"));
    auto *catTagAdd = new QPushButton(tr("Assign"), this);
    auto *catTagDel = new QPushButton(tr("Remove"), this);
    catTagRow->addWidget(m_tagCustomEdit, 1);
    catTagRow->addWidget(m_tagCustomCategory, 1);
    catTagRow->addWidget(catTagAdd);
    catTagRow->addWidget(catTagDel);
    typesLay->addLayout(catTagRow);
    auto refreshCatCombo = [this]() {
        if (!m_tagCustomCategory)
            return;
        const QString keep = m_tagCustomCategory->currentText();
        m_tagCustomCategory->clear();
        for (int i = 0; i < m_customCategories->count(); ++i)
            m_tagCustomCategory->addItem(
                m_customCategories->item(i)->text());
        if (!keep.isEmpty())
            m_tagCustomCategory->setEditText(keep);
    };
    refreshCatCombo();
    connect(customCatAdd, &QPushButton::clicked, this,
            [this, refreshCatCombo] {
        const QString cat = m_customCategoryEdit->text().trimmed();
        if (cat.isEmpty())
            return;
        for (int i = 0; i < m_customCategories->count(); ++i) {
            if (m_customCategories->item(i)->text().compare(
                    cat, Qt::CaseInsensitive) == 0) {
                m_customCategoryEdit->clear();
                return;
            }
        }
        new QListWidgetItem(cat, m_customCategories);
        m_customCategoryEdit->clear();
        refreshCatCombo();
        updateApplyState();
    });
    connect(customCatDel, &QPushButton::clicked, this,
            [this, refreshCatCombo] {
        const int row = m_customCategories->currentRow();
        if (row < 0)
            return;
        const QString cat = m_customCategories->item(row)->text();
        delete m_customCategories->takeItem(row);
        for (int i = m_tagCustomAssignments->count() - 1; i >= 0; --i) {
            if (m_tagCustomAssignments->item(i)
                    ->data(Qt::UserRole + 1)
                    .toString()
                    .compare(cat, Qt::CaseInsensitive) == 0)
                delete m_tagCustomAssignments->takeItem(i);
        }
        refreshCatCombo();
        updateApplyState();
    });
    connect(catTagAdd, &QPushButton::clicked, this,
            [this, refreshCatCombo] {
        const QString tag = m_tagCustomEdit->text().trimmed().toLower();
        const QString cat = m_tagCustomCategory->currentText().trimmed();
        if (tag.isEmpty() || cat.isEmpty())
            return;
        bool haveCat = false;
        for (int i = 0; i < m_customCategories->count(); ++i)
            if (m_customCategories->item(i)->text().compare(
                    cat, Qt::CaseInsensitive) == 0)
                haveCat = true;
        if (!haveCat)
            new QListWidgetItem(cat, m_customCategories);
        for (int i = 0; i < m_tagCustomAssignments->count(); ++i) {
            QListWidgetItem *item = m_tagCustomAssignments->item(i);
            if (item->data(Qt::UserRole).toString() == tag) {
                item->setText(tr("%1 → %2").arg(tag, cat));
                item->setData(Qt::UserRole + 1, cat);
                m_tagCustomEdit->clear();
                refreshCatCombo();
                updateApplyState();
                return;
            }
        }
        auto *item = new QListWidgetItem(
            tr("%1 → %2").arg(tag, cat), m_tagCustomAssignments);
        item->setData(Qt::UserRole, tag);
        item->setData(Qt::UserRole + 1, cat);
        m_tagCustomEdit->clear();
        refreshCatCombo();
        updateApplyState();
    });
    connect(catTagDel, &QPushButton::clicked, this, [this] {
        delete m_tagCustomAssignments->takeItem(
            m_tagCustomAssignments->currentRow());
        updateApplyState();
    });
    tagsPage->addWidget(types);

    // ── Merge tag categories: one group/rename per booru ──
    auto *merge = new QGroupBox(tr("Merge tag categories"), this);
    auto *mergeLay = new QVBoxLayout(merge);
    mergeLay->addWidget(note(tr(
        "Group a booru's native categories under one label, or rename a "
        "classic one. Sources accept commas or spaces; `*` matches every "
        "category. Examples: `character, variant -> Characters`, "
        "`* -> soybooru_specific`.")));
    auto *mergeRow = new QHBoxLayout();
    m_mergeScope = new QComboBox(this);
    m_mergeScope->addItem(tr("All boorus"), QString());
    for (const BooruSite &b : current.boorus) {
        if (b.enabled && !b.key.isEmpty())
            m_mergeScope->addItem(b.label.isEmpty() ? b.key : b.label,
                                  b.key);
    }
    m_mergeSources = new QLineEdit(this);
    m_mergeSources->setPlaceholderText(
        tr("character, variant, subvariant"));
    m_mergeTarget = new QLineEdit(this);
    m_mergeTarget->setPlaceholderText(tr("Characters / custom label"));
    auto *mergeAdd = new QPushButton(tr("Add"), this);
    auto *mergeDel = new QPushButton(tr("Remove"), this);
    mergeRow->addWidget(m_mergeScope);
    mergeRow->addWidget(m_mergeSources, 2);
    mergeRow->addWidget(new QLabel(QStringLiteral("→"), this));
    mergeRow->addWidget(m_mergeTarget, 1);
    mergeRow->addWidget(mergeAdd);
    mergeRow->addWidget(mergeDel);
    mergeLay->addLayout(mergeRow);
    m_categoryMerges = new QListWidget(this);
    m_categoryMerges->setMinimumHeight(90);
    mergeLay->addWidget(m_categoryMerges);
    auto refreshMergeLabel = [this](QListWidgetItem *it) {
        if (!it)
            return;
        const QString scope = it->data(Qt::UserRole).toString();
        const QString site = scope.isEmpty()
                                 ? tr("All boorus")
                                 : (m_mergeScope->findData(scope) >= 0
                                        ? m_mergeScope->itemText(
                                              m_mergeScope->findData(scope))
                                        : scope);
        it->setText(tr("%1: %2 → %3")
                        .arg(site,
                             it->data(Qt::UserRole + 1).toString(),
                             it->data(Qt::UserRole + 2).toString()));
    };
    for (const TagCategoryMerge &m : current.tagCategoryMerges) {
        auto *it = new QListWidgetItem(m_categoryMerges);
        it->setData(Qt::UserRole, m.siteKey);
        it->setData(Qt::UserRole + 1, m.sources.join(QStringLiteral(", ")));
        it->setData(Qt::UserRole + 2, m.target);
        refreshMergeLabel(it);
    }
    connect(mergeAdd, &QPushButton::clicked, this, [this, refreshMergeLabel] {
        QStringList sources;
        for (const QString &s : m_mergeSources->text().split(
                 QRegularExpression(QStringLiteral("[,\\s]+")),
                 Qt::SkipEmptyParts)) {
            if (!s.trimmed().isEmpty())
                sources << s.trimmed();
        }
        const QString target = m_mergeTarget->text().trimmed();
        if (sources.isEmpty() || target.isEmpty())
            return;
        auto *it = new QListWidgetItem(m_categoryMerges);
        it->setData(Qt::UserRole, m_mergeScope->currentData().toString());
        it->setData(Qt::UserRole + 1, sources.join(QStringLiteral(", ")));
        it->setData(Qt::UserRole + 2, target);
        refreshMergeLabel(it);
        m_mergeSources->clear();
        m_mergeTarget->clear();
        updateApplyState();
    });
    connect(mergeDel, &QPushButton::clicked, this, [this] {
        delete m_categoryMerges->takeItem(m_categoryMerges->currentRow());
        updateApplyState();
    });
    tagsPage->addWidget(merge);

    // ── Browse ──
    auto *browse = new QGroupBox(tr("Browse && downloads"), this);
    auto *browseLay = new QFormLayout(browse);
    browseLay->setRowWrapPolicy(QFormLayout::WrapLongRows);
    browseLay->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    m_browseLimit = new QComboBox(this);
    for (int n : {20, 40, 60, 100})
        m_browseLimit->addItem(tr("%1 per page").arg(n), n);
    m_browseLimit->setCurrentIndex(
        qMax(0, m_browseLimit->findData(current.browseLimit)));
    browseLay->addRow(tr("Page size:"), m_browseLimit);
    m_dragMode = new QComboBox(this);
    m_dragMode->addItem(tr("Post page link"),
                        int(DragLinkMode::PostPage));
    m_dragMode->addItem(tr("Direct media link"),
                        int(DragLinkMode::DirectMedia));
    m_dragMode->setCurrentIndex(qMax(
        0, m_dragMode->findData(int(current.dragLinkMode))));
    m_dragMode->setToolTip(
        tr("What dragging a remote post into Discord (or any other app) "
           "sends. Post links open the booru page and always hotlink "
           "correctly; direct links skip the page. Per-booru overrides "
           "live in the Booru manager below."));
    connect(m_dragMode, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &PrefsDialog::updateApplyState);
    browseLay->addRow(tr("Drag posts as:"), m_dragMode);
    m_defBoorus = new QListWidget(this);
    m_defBoorus->setMinimumHeight(110);
    const QList<BooruSite> tabBoorus =
        current.boorus.isEmpty() ? RemoteSource::presetBoorus() : current.boorus;
    for (const BooruSite &s : tabBoorus) {
        if (!s.enabled)
            continue;
        auto *item = new QListWidgetItem(s.label, m_defBoorus);
        item->setData(Qt::UserRole, s.key);
        item->setCheckState(current.browseSites.contains(s.key) ? Qt::Checked
                                                               : Qt::Unchecked);
    }
    browseLay->addRow(tr("Default boorus:"), m_defBoorus);
    m_dataDefault = defaultDataDir();
    m_dataDir = new QLineEdit(
        current.dataDir.isEmpty() ? m_dataDefault : current.dataDir, this);
    auto *dataBrowse = new QPushButton(tr("Browse…"), this);
    connect(dataBrowse, &QPushButton::clicked, this, [this] {
        const QString d = QFileDialog::getExistingDirectory(
            this, tr("Data folder"), m_dataDir->text());
        if (!d.isEmpty())
            m_dataDir->setText(d);
    });
    auto *dataRow = new QHBoxLayout();
    dataRow->addWidget(m_dataDir, 1);
    dataRow->addWidget(dataBrowse);
    browseLay->addRow(tr("Data folder:"), dataRow);
    browseLay->addRow(QStringLiteral(""), note(tr(
        "Favourites and tag verdicts live here (store.db, "
        "tag_types.db). Changing it starts fresh there - move "
        "your existing files over to keep them.")));
    m_downloadDefault = effectiveDownloadDir(current);
    m_downloadDir = new QLineEdit(
        current.downloadDir.isEmpty() ? m_downloadDefault : current.downloadDir,
        this);
    auto *dlBrowse = new QPushButton(tr("Browse…"), this);
    connect(dlBrowse, &QPushButton::clicked, this, [this] {
        const QString d = QFileDialog::getExistingDirectory(
            this, QStringLiteral("Download folder"), m_downloadDir->text());
        if (!d.isEmpty())
            m_downloadDir->setText(d);
    });
    auto *dlRow = new QHBoxLayout();
    dlRow->addWidget(m_downloadDir, 1);
    dlRow->addWidget(dlBrowse);
    browseLay->addRow(tr("Download folder:"), dlRow);
    browseLay->addRow(QStringLiteral(""), note(QStringLiteral(
        "Defaults to downloads/ inside the data folder. Downloads save full "
        "quality as <Site>_<md5>.<ext> plus the raw API "
        "JSON sidecar, so they browse seamlessly offline.")));
    m_ugoiraFormat = new QComboBox(this);
    m_ugoiraFormat->addItem(tr("Animated WebP"), QStringLiteral("webp"));
    m_ugoiraFormat->addItem(tr("Animated JXL"), QStringLiteral("jxl"));
    m_ugoiraFormat->addItem(tr("Keep original ZIP"), QStringLiteral("zip"));
    m_ugoiraFormat->setCurrentIndex(
        qMax(0, m_ugoiraFormat->findData(current.ugoiraFormat)));
    m_ugoiraFormat->setToolTip(tr("Ugoira posts download as .zip frame "
                                  "packs; convert them on arrival (needs "
                                  "ffmpeg in PATH, +cjxl for JXL). JXL needs "
                                  "a Qt JXL image plugin to preview in-app."));
    connect(m_ugoiraFormat,
            QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &PrefsDialog::updateApplyState);
    browseLay->addRow(tr("Ugoira downloads:"), m_ugoiraFormat);
    m_ugoiraDelay = new QSpinBox(this);
    m_ugoiraDelay->setRange(20, 2000);
    m_ugoiraDelay->setSuffix(tr(" ms"));
    m_ugoiraDelay->setValue(current.ugoiraFrameDelayMs);
    m_ugoiraDelay->setToolTip(tr("Per-frame delay used when the API omits "
                                 "pixiv frame timing."));
    connect(m_ugoiraDelay, QOverload<int>::of(&QSpinBox::valueChanged), this,
            &PrefsDialog::updateApplyState);
    browseLay->addRow(tr("Ugoira frame delay:"), m_ugoiraDelay);
    m_globalProxy = new QLineEdit(current.globalProxy, this);
    m_globalProxy->setPlaceholderText(QStringLiteral("socks5h://127.0.0.1:9050"));
    m_globalProxy->setToolTip(tr("Global proxy for all traffic "
                                "(http:// or socks5:// host:port). SOCKS5 routes "
                                "the hostname through the proxy too, like curl "
                                "socks5h - nothing leaks to local DNS. A per-booru "
                                "proxy overrides this one; \"direct\" forces that "
                                "booru direct. Empty = direct. The embedded "
                                "browser (capture, PoW walls, onion sites) "
                                "reads it at startup - restart after changing it."));
    connect(m_globalProxy, &QLineEdit::textChanged, this,
            &PrefsDialog::updateApplyState);
    browseLay->addRow(tr("Global proxy:"), m_globalProxy);
    m_wallAutoPopup = new QCheckBox(
        tr("Pop the browser open on captcha/wall blocks"), this);
    m_wallAutoPopup->setToolTip(
        tr("Fresh searches that hit a captcha/PoW wall open the solve "
           "browser immediately. Off = just the wall menu."));
    m_wallAutoPopup->setChecked(current.wallAutoPopup);
    browseLay->addRow(QStringLiteral(""), m_wallAutoPopup);
    browseLay->addRow(QStringLiteral(""), note(tr(
        "Applies to searches, media, tags, favicons and downloads. "
        "A proxy set on one booru (below) wins for that booru only.")));
    browsePage->addWidget(browse);

    // ── Booru manager: presets + user-added instances ──
    auto *acc = new QGroupBox(tr("Booru manager"), this);
    auto *accLay = new QHBoxLayout(acc);
    auto *accLeft = new QVBoxLayout();
    m_booruList = new QListWidget(this);
    m_booruList->setFixedWidth(150);
    m_booruEdits = current.boorus;
    accLeft->addWidget(m_booruList, 1);
    auto *accBtnRow = new QHBoxLayout();
    auto *booruAdd = new QPushButton(tr("Add"), this);
    auto *booruDel = new QPushButton(tr("Remove"), this);
    connect(booruAdd, &QPushButton::clicked, this, [this] {
        saveBooruEditor();
        QSet<QString> taken;
        for (const BooruSite &b : m_booruEdits)
            taken.insert(b.key);
        BooruSite nb;
        nb.key = RemotePosts::uniqueBooruKey(QStringLiteral("New booru"), taken);
        nb.label = tr("New booru");
        nb.shortName = RemotePosts::shortFor(nb.label);
        nb.engine = BooruEngine::Danbooru;
        m_booruEdits.append(nb);
        refreshBooruList();
        m_booruList->setCurrentRow(m_booruList->count() - 1);
        m_booruName->setFocus();
        m_booruName->selectAll();
        updateApplyState();
    });
    connect(booruDel, &QPushButton::clicked, this, [this] {
        const int row = m_booruList->currentRow();
        if (row < 0 || row >= m_booruEdits.size())
            return;
        m_booruEdits.removeAt(row);
        refreshBooruList();
        updateApplyState();
    });
    auto *booruDefaults = new QPushButton(tr("Restore presets"), this);
    connect(booruDefaults, &QPushButton::clicked, this, [this] {
        // Destructive-ish (list order and hidden presets change), so the
        // user must confirm three times with increasingly explicit text.
        const QString key = tr("Restore presets");
        if (QMessageBox::question(
                this, key,
                tr("Re-add the shipped preset boorus?"),
                QMessageBox::Yes | QMessageBox::No,
                QMessageBox::No) != QMessageBox::Yes)
            return;
        if (QMessageBox::warning(
                this, key,
                tr("This changes your booru list. Custom entries are kept, "
                   "but preset entries you removed will come back.\n\n"
                   "Continue?"),
                QMessageBox::Yes | QMessageBox::No,
                QMessageBox::No) != QMessageBox::Yes)
            return;
        if (QMessageBox::warning(
                this, key,
                tr("Last check: really restore the presets now?"),
                QMessageBox::Yes | QMessageBox::No,
                QMessageBox::No) != QMessageBox::Yes)
            return;
        // Keep user entries; re-add any missing presets (with their
        // credentials preserved when still present).
        QSet<QString> have;
        for (const BooruSite &b : m_booruEdits)
            have.insert(b.key);
        for (const BooruSite &p : RemoteSource::presetBoorus()) {
            if (!have.contains(p.key))
                m_booruEdits.append(p);
        }
        refreshBooruList();
        updateApplyState();
    });
    auto *booruUp = new QPushButton(tr("↑"), this);
    booruUp->setToolTip(tr("Move this booru up"));
    auto *booruDown = new QPushButton(tr("↓"), this);
    booruDown->setToolTip(tr("Move this booru down"));
    auto moveBooru = [this](int delta) {
        const int row = m_booruList->currentRow();
        const int dst = row + delta;
        if (row < 0 || dst < 0 || dst >= m_booruEdits.size())
            return;
        m_booruEdits.move(row, dst);
        refreshBooruList();
        m_booruList->setCurrentRow(dst);
        updateApplyState();
    };
    connect(booruUp, &QPushButton::clicked, this,
            [moveBooru] { moveBooru(-1); });
    connect(booruDown, &QPushButton::clicked, this,
            [moveBooru] { moveBooru(1); });
    accBtnRow->addWidget(booruAdd);
    accBtnRow->addWidget(booruDel);
    accBtnRow->addWidget(booruUp);
    accBtnRow->addWidget(booruDown);
    accLeft->addLayout(accBtnRow);
    accLeft->addWidget(booruDefaults);
    auto *faviconRefresh = new QPushButton(tr("Refresh favicons"), this);
    faviconRefresh->setToolTip(tr("Drop cached site icons and fetch them again"));
    connect(faviconRefresh, &QPushButton::clicked, this,
            &PrefsDialog::faviconsRefreshRequested);
    accLeft->addWidget(faviconRefresh);
    accLay->addLayout(accLeft);
    m_booruEmpty = new QLabel(tr("Select a booru on the left to edit it."), this);
    m_booruEmpty->setWordWrap(true);
    m_booruEmpty->setStyleSheet(QStringLiteral("color:#4a5068;"));
    m_booruEmpty->setVisible(false);
    accLay->addWidget(m_booruEmpty);
    auto *accFormBox = new QWidget(this);
    accFormBox->setMinimumWidth(0);
    auto *accForm = new QFormLayout(accFormBox);
    // Long rows (cookie buttons, notes) wrap under their label instead of
    // pushing the whole page wider than the dialog.
    accForm->setRowWrapPolicy(QFormLayout::WrapLongRows);
    accForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    m_booruName = new QLineEdit(this);
    m_booruBase = new QLineEdit(this);
    m_booruBase->setPlaceholderText(QStringLiteral("https://…"));
    m_booruEngine = new QComboBox(this);
    m_booruEngine->addItem(RemotePosts::engineLabel(BooruEngine::Danbooru),
                           RemotePosts::engineKey(BooruEngine::Danbooru));
    m_booruEngine->addItem(RemotePosts::engineLabel(BooruEngine::Gelbooru),
                           RemotePosts::engineKey(BooruEngine::Gelbooru));
    m_booruEngine->addItem(RemotePosts::engineLabel(BooruEngine::Moebooru),
                           RemotePosts::engineKey(BooruEngine::Moebooru));
    m_booruEngine->addItem(RemotePosts::engineLabel(BooruEngine::Szurubooru),
                           RemotePosts::engineKey(BooruEngine::Szurubooru));
    m_booruEngine->addItem(RemotePosts::engineLabel(BooruEngine::Shimmie),
                           RemotePosts::engineKey(BooruEngine::Shimmie));
    m_booruEngine->addItem(RemotePosts::engineLabel(BooruEngine::Hybooru),
                           RemotePosts::engineKey(BooruEngine::Hybooru));
    m_booruEngine->addItem(RemotePosts::engineLabel(BooruEngine::Nuuru),
                           RemotePosts::engineKey(BooruEngine::Nuuru));
    m_booruEngine->addItem(RemotePosts::engineLabel(BooruEngine::Derpibooru),
                           RemotePosts::engineKey(BooruEngine::Derpibooru));
    m_booruEngine->addItem(RemotePosts::engineLabel(BooruEngine::Twibooru),
                           RemotePosts::engineKey(BooruEngine::Twibooru));
    m_booruUser = new QLineEdit(this);
    m_booruSecret = new QLineEdit(this);
    m_booruSecret->setEchoMode(QLineEdit::Password);
    m_booruNoClassify = new QCheckBox(tr("Skip automatic tag categorization"), this);
    m_booruNoClassify->setToolTip(tr("Tags from this booru stay as they arrive "
                                     "(for meme hosts and the like)."));
    m_booruLocalBlacklist =
        new QCheckBox(tr("Filter blacklist locally (no server -tags)"), this);
    m_booruLocalBlacklist->setToolTip(
        tr("Skip server-side blacklist exclusions for this booru and "
           "filter locally instead (for tag-capped accounts, e.g. free "
           "Danbooru)."));
    m_booruTags = new QLineEdit(this);
    m_booruTags->setPlaceholderText(tr("score:>10 -comic"));
    m_booruTags->setToolTip(tr("Extra tags for every search on this booru."));
    m_booruProxy = new QLineEdit(this);
    m_booruProxy->setPlaceholderText(tr("http://127.0.0.1:8080"));
    m_booruProxy->setToolTip(tr("Proxy for this booru's traffic "
                                "(http:// or socks5://; empty = global proxy, "
                                "\"direct\" = bypass it)."));
    accForm->addRow(tr("Name:"), m_booruName);
    m_booruEnabled = new QCheckBox(tr("Enabled (hide from pickers when off)"),
                                   this);
    m_booruEnabled->setToolTip(
        tr("Keeps the entry and its credentials, but hides this booru from "
           "new-tab, source and default-booru menus."));
    connect(m_booruEnabled, &QCheckBox::toggled, this,
            [this] { updateApplyState(); });
    accForm->addRow(QString(), m_booruEnabled);
    m_booruShort = new QLineEdit(this);
    m_booruShort->setMaxLength(6);
    m_booruShort->setToolTip(tr("Short text on grid badges"));
    accForm->addRow(tr("Badge text:"), m_booruShort);
    m_booruColor = QColor(QStringLiteral("#3a7ec2"));
    m_booruColorBtn = new QPushButton(this);
    setBooruSwatch();
    connect(m_booruColorBtn, &QPushButton::clicked, this, [this] {
        const QColor c = QColorDialog::getColor(m_booruColor, this,
                                               tr("Badge color"));
        if (c.isValid()) {
            m_booruColor = c;
            setBooruSwatch();
            updateApplyState();
        }
    });
    accForm->addRow(tr("Badge color:"), m_booruColorBtn);
    accForm->addRow(tr("Base URL:"), m_booruBase);
    auto *typeBox = new QWidget(this);
    {
        auto *booruTypeRow = new QHBoxLayout(typeBox);
        booruTypeRow->setContentsMargins(0, 0, 0, 0);
        m_booruEngine->setSizePolicy(QSizePolicy::Expanding,
                                     QSizePolicy::Fixed);
        booruTypeRow->addWidget(m_booruEngine, 1);
        auto *detectBtn = new QPushButton(tr("Detect"), typeBox);
        detectBtn->setToolTip(
            tr("Probe the instance URL and select the matching engine"));
        booruTypeRow->addWidget(detectBtn);
        connect(detectBtn, &QPushButton::clicked, this,
                [this, detectBtn] { detectBooruEngine(detectBtn); });
    }
    accForm->addRow(tr("Type:"), typeBox);
    accForm->addRow(tr("Tags:"), m_booruNoClassify);
    accForm->addRow(tr("Blacklist:"), m_booruLocalBlacklist);
    accForm->addRow(tr("Default tags:"), m_booruTags);
    m_booruDragMode = new QComboBox(this);
    m_booruDragMode->addItem(tr("Use global"),
                             int(DragLinkMode::Inherit));
    m_booruDragMode->addItem(tr("Direct media link"),
                             int(DragLinkMode::DirectMedia));
    m_booruDragMode->addItem(tr("Post page link"),
                             int(DragLinkMode::PostPage));
    m_booruDragMode->setToolTip(
        tr("Overrides the global drag setting for this booru. Leave on "
           "Use global unless this host's media rejects hotlinks."));
    connect(m_booruDragMode,
            QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this] { updateApplyState(); });
    accForm->addRow(tr("Drag posts as:"), m_booruDragMode);
    auto *cacheBox = new QWidget(this);
    {
        auto *booruCacheRow = new QHBoxLayout(cacheBox);
        booruCacheRow->setContentsMargins(0, 0, 0, 0);
        m_booruCacheInherit = new QRadioButton(tr("Global"), cacheBox);
        m_booruCacheAll = new QRadioButton(tr("All"), cacheBox);
        m_booruCacheSfw = new QRadioButton(tr("SFW only"), cacheBox);
        m_booruCacheNone = new QRadioButton(tr("Nothing"), cacheBox);
        auto *group = new QButtonGroup(cacheBox);
        for (QRadioButton *r : {m_booruCacheInherit, m_booruCacheAll,
                                m_booruCacheSfw, m_booruCacheNone}) {
            group->addButton(r);
            booruCacheRow->addWidget(r);
        }
        booruCacheRow->addStretch(1);
        m_booruCacheInherit->setToolTip(
            tr("Use the global media cache mode"));
        m_booruCacheAll->setToolTip(
            tr("Cache this booru's media on disk (still limited by the "
               "global mode)"));
        m_booruCacheSfw->setToolTip(
            tr("Cache only this booru's SFW pictures/videos"));
        m_booruCacheNone->setToolTip(
            tr("Keep this booru's media in memory/session only"));
        m_booruCacheInherit->setChecked(true);
    }
    accForm->addRow(tr("Media cache:"), cacheBox);
    accForm->addRow(tr("Proxy:"), m_booruProxy);
    accForm->addRow(tr("User / login:"), m_booruUser);
    accForm->addRow(tr("API key / password:"), m_booruSecret);
    m_booruCookieLabel = new QLabel(this);
    auto *cookieEdit = new QPushButton(tr("Edit…"), this);
    connect(cookieEdit, &QPushButton::clicked, this, [this] {
        saveBooruEditor();
        BooruSite *e = currentBooruEntry();
        if (!e)
            return;
        CookieEditorDialog dlg(e->cookies, this);
        if (dlg.exec() == QDialog::Accepted) {
            e->cookies = dlg.cookies();
            updateCookieLabel();
            updateApplyState();
        }
    });
    auto *cookieGrab = new QPushButton(tr("Capture in browser…"), this);
    cookieGrab->setToolTip(tr("Open a login browser for this booru, solve "
                              "any captcha/Cloudflare challenge, then harvest "
                              "the session cookies."));
    connect(cookieGrab, &QPushButton::clicked, this, [this] {
        saveBooruEditor();
        BooruSite *e = currentBooruEntry();
        if (!e)
            return;
        if (e->baseUrl.isEmpty()) {
            m_accStatus->setText(
                tr("✗ %1 needs an instance URL first").arg(e->label));
            return;
        }
        QString dataDir = m_dataDir->text().trimmed();
        if (dataDir.isEmpty())
            dataDir = defaultDataDir();
        CookieGrabDialog dlg(e->label, e->baseUrl,
                             dataDir + QStringLiteral("/webprofile"), this);
        if (dlg.exec() == QDialog::Accepted && !dlg.cookies().isEmpty()) {
            e->cookies = dlg.cookies();
            updateCookieLabel();
            updateApplyState();
            m_accStatus->setText(
                tr("✓ %1: %2 cookie(s) captured").arg(e->label).arg(e->cookies.size()));
        }
    });
    auto *cookieRefresh = new QPushButton(tr("Refresh"), this);
    cookieRefresh->setToolTip(tr("Reload the site in a hidden browser to "
                                 "renew short-lived challenge cookies "
                                 "(proof-of-work walls rotate them even when "
                                 "logged in) - no puzzle to solve."));
    connect(cookieRefresh, &QPushButton::clicked, this, [this, cookieRefresh] {
        saveBooruEditor();
        BooruSite *e = currentBooruEntry();
        if (!e)
            return;
        if (e->baseUrl.isEmpty()) {
            m_accStatus->setText(
                tr("✗ %1 needs an instance URL first").arg(e->label));
            return;
        }
        QString dataDir = m_dataDir->text().trimmed();
        if (dataDir.isEmpty())
            dataDir = defaultDataDir();
        // The refresh outlives this click by seconds: re-resolve the
        // entry on arrival (the dialog or row may be gone by then).
        QPointer<PrefsDialog> self(this);
        QPointer<QPushButton> btn(cookieRefresh);
        const QString entryKey = e->key;
        const QString baseUrl = e->baseUrl;
        const QString label = e->label;
        cookieRefresh->setEnabled(false);
        m_accStatus->setText(tr("↻ %1: reloading session…").arg(label));
        CookieGrabDialog::refreshSilent(
            baseUrl, dataDir + QStringLiteral("/webprofile"),
            [self, btn, entryKey, label](const QStringList &fresh) {
                if (self.isNull())
                    return;
                if (!btn.isNull())
                    btn->setEnabled(true);
                if (fresh.isEmpty()) {
                    self->m_accStatus->setText(
                        tr("✗ %1: still challenged - open Capture in "
                           "browser… and solve it.").arg(label));
                    return;
                }
                BooruSite *live = nullptr;
                for (BooruSite &b : self->m_booruEdits) {
                    if (b.key == entryKey) {
                        live = &b;
                        break;
                    }
                }
                if (!live)
                    return;
                live->cookies = fresh;
                self->updateCookieLabel();
                self->updateApplyState();
                self->m_accStatus->setText(
                    tr("✓ %1: %2 cookie(s) refreshed - Apply to save.")
                        .arg(label).arg(fresh.size()));
            });
    });
    // Wrapping layout: three fixed-size buttons + the "N set"/"none"
    // label are wider than the editor column, so a plain HBox overflowed
    // past the (horizontally unscrollable) page edge and clipped the
    // "Capture in browser…" button. FlowLayout wraps to a second line.
    auto *cookieBox = new QWidget(this);
    auto *cookieFlow = new FlowLayout(cookieBox, 0, 6, 4);
    cookieFlow->addWidget(m_booruCookieLabel);
    cookieFlow->addWidget(cookieEdit);
    cookieFlow->addWidget(cookieGrab);
    cookieFlow->addWidget(cookieRefresh);
    accForm->addRow(tr("Cookies:"), cookieBox);
    // Custom favicon for boorus whose icon refuses to fetch (or have
    // none): staged here, converted to <dataDir>/favicons/<key>.png on
    // Apply. Cancel touches nothing.
    m_booruFavicon = new QLabel(this);
    m_booruFavicon->setFixedSize(16, 16);
    m_booruFavicon->setScaledContents(true);
    auto *favChoose = new QPushButton(tr("Choose…"), this);
    favChoose->setToolTip(tr("Use an image file as this booru's icon"));
    connect(favChoose, &QPushButton::clicked, this, [this] {
        saveBooruEditor();
        BooruSite *e = currentBooruEntry();
        if (!e)
            return;
        const QString src = QFileDialog::getOpenFileName(
            this, tr("Custom favicon - %1").arg(e->label),
            QDir::homePath(),
            tr("Images (*.png *.jpg *.jpeg *.ico *.svg *.webp);;All files (*)"));
        if (src.isEmpty())
            return;
        m_pendingFavicons.insert(e->key, src);
        updateFaviconPreview();
        updateApplyState();
    });
    auto *favClear = new QPushButton(tr("Clear"), this);
    favClear->setToolTip(tr("Drop the custom icon (auto-fetch again)"));
    connect(favClear, &QPushButton::clicked, this, [this] {
        saveBooruEditor();
        BooruSite *e = currentBooruEntry();
        if (!e)
            return;
        m_pendingFavicons.insert(e->key, QString());
        updateFaviconPreview();
        updateApplyState();
    });
    auto *favRow = new QHBoxLayout();
    favRow->addWidget(m_booruFavicon);
    favRow->addStretch(1);
    favRow->addWidget(favChoose);
    favRow->addWidget(favClear);
    auto *favBox = new QWidget(this);
    favBox->setLayout(favRow);
    accForm->addRow(tr("Favicon:"), favBox);
    auto *testBtn = new QPushButton(tr("Test"), this);
    connect(testBtn, &QPushButton::clicked, this, [this] {
        saveBooruEditor();
        const int row = m_booruList->currentRow();
        if (row < 0 || row >= m_booruEdits.size())
            return;
        testSite(m_booruEdits.at(row));
    });
    // Compact button (same stretch fix as Clear search history): a bare
    // button as a form field grows to the full editor width.
    auto *testRow = new QHBoxLayout();
    testRow->addWidget(testBtn);
    testRow->addStretch(1);
    accForm->addRow(QStringLiteral(""), testRow);
    accForm->addRow(QStringLiteral(""), note(tr(
        "Keys and cookies are stored plaintext in settings.ini. Renaming a booru orphans "
        "its old favourites entries (they keep the old name); the key itself "
        "never changes once created.")));
    accLay->addWidget(accFormBox, 1);
    m_booruFormBox = accFormBox;
    connect(m_booruList, &QListWidget::currentItemChanged, this,
            [this](QListWidgetItem *cur, QListWidgetItem *prev) {
                if (prev)
                    commitBooruEditor(prev->data(Qt::UserRole).toString());
                if (cur)
                    loadBooruEditor(m_booruList->row(cur));
                refreshBooruListLabels();
                updateApplyState();
            });
    browsePage->addWidget(acc);
    m_accStatus = new QLabel(this);
    m_accStatus->setStyleSheet(QStringLiteral("color:#4a5068;font-size:11px;"));
    m_accStatus->setWordWrap(true);
    // Long Test responses (unbroken server bodies) must wrap inside the
    // dialog, never widen it: the layout decides the width, not the text.
    m_accStatus->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_accStatus->installEventFilter(this); // click -> full response viewer
    browsePage->addWidget(m_accStatus);
    if (m_favicons) {
        // Auto icon arriving late (first fetch): show it in the preview
        // box when it belongs to the entry on screen.
        connect(m_favicons, &FaviconCache::ready, this,
                [this](const QString &key, const QIcon &) {
                    const BooruSite *e = currentBooruEntry();
                    if (e && e->key == key)
                        updateFaviconPreview();
                });
    }
    refreshBooruList();
    m_booruList->setCurrentRow(0);

    // ── Appearance ──
    auto *app = new QGroupBox(tr("Appearance"), this);
    auto *appLay = new QFormLayout(app);
    m_themeMode = new QComboBox(this);
    for (const Theme::ThemeInfo &info : Theme::availableThemes())
        m_themeMode->addItem(info.label, info.key);
    m_themeMode->addItem(tr("System native"), QStringLiteral("native"));
    int themeIdx = m_themeMode->findData(current.themeMode);
    if (themeIdx < 0) {
        // A removed custom theme: keep the key visible instead of silently
        // switching the user to the first entry.
        m_themeMode->addItem(tr("%1 (missing file)").arg(current.themeMode),
                             current.themeMode);
        themeIdx = m_themeMode->count() - 1;
    }
    m_themeMode->setCurrentIndex(qMax(0, themeIdx));
    appLay->addRow(tr("Theme:"), m_themeMode);
    m_accentBtn = new QPushButton(tr("Pick…"), this);
    setSwatch(m_accent);
    connect(m_accentBtn, &QPushButton::clicked, this, [this] {
        const QColor c =
            QColorDialog::getColor(m_accent, this, QStringLiteral("Accent color"));
        if (c.isValid()) {
            m_accent = c;
            setSwatch(c);
            updateApplyState();
        }
    });
    auto *accRow = new QHBoxLayout();
    accRow->addWidget(m_accentBtn);
    accRow->addWidget(new QLabel(
        tr("Highlights, selections, progress bars"), this));
    accRow->addStretch(1);
    appLay->addRow(tr("Accent:"), accRow);
    m_stylesheet = new QLineEdit(current.stylesheet, this);
    m_stylesheet->setPlaceholderText(QStringLiteral("Optional custom .qss"));
    auto *ssBrowse = new QPushButton(tr("Browse…"), this);
    connect(ssBrowse, &QPushButton::clicked, this, [this] {
        const QString p = QFileDialog::getOpenFileName(
            this, QStringLiteral("Custom stylesheet"), m_stylesheet->text(),
            tr("Stylesheets (*.qss);;All files (*)"));
        if (!p.isEmpty())
            m_stylesheet->setText(p);
    });
    auto *ssClear = new QPushButton(tr("Clear"), this);
    connect(ssClear, &QPushButton::clicked, m_stylesheet, &QLineEdit::clear);
    auto *ssRow = new QHBoxLayout();
    ssRow->addWidget(m_stylesheet, 1);
    ssRow->addWidget(ssBrowse);
    ssRow->addWidget(ssClear);
    appLay->addRow(tr("Custom .qss:"), ssRow);
    m_gridSnap = new QCheckBox(
        tr("Snap grid post size to fill the window (no right gap)"), this);
    m_gridSnap->setChecked(current.gridSnap);
    m_gridSnap->setToolTip(
        tr("When on, the grid card width stretches so a whole number of "
           "columns fills the viewport. Turn off to keep the exact size "
           "from the zoom slider (leaving the leftover strip)."));
    connect(m_gridSnap, &QCheckBox::toggled, this,
            &PrefsDialog::updateApplyState);
    appLay->addRow(QString(), m_gridSnap);
    m_gridStaggered = new QCheckBox(
        tr("Staggered grid rows (brick layout)"), this);
    m_gridStaggered->setChecked(current.gridStaggered);
    m_gridStaggered->setToolTip(
        tr("Shifts every other column down by half a card. Off by "
           "default."));
    connect(m_gridStaggered, &QCheckBox::toggled, this,
            &PrefsDialog::updateApplyState);
    appLay->addRow(QString(), m_gridStaggered);
    m_sidebarSingleScroll = new QCheckBox(
        tr("Sidebar: one scrollbar for all tag categories"), this);
    m_sidebarSingleScroll->setChecked(current.sidebarSingleScroll);
    m_sidebarSingleScroll->setToolTip(
        tr("Default: each category scrolls on its own. On: the whole "
           "sidebar scrolls together with one scrollbar."));
    appLay->addRow(QString(), m_sidebarSingleScroll);
    m_plusLocalFolder = new QCheckBox(
        tr("Show local folder browser in the quick + menu"), this);
    m_plusLocalFolder->setChecked(current.showLocalFolderInPlusMenu);
    m_plusLocalFolder->setToolTip(
        tr("Adds an \"Open folder…\" entry next to Home/Favourites/"
           "Downloads in the tab-bar + menu."));
    connect(m_plusLocalFolder, &QCheckBox::toggled, this,
            [this] { updateApplyState(); });
    appLay->addRow(QString(), m_plusLocalFolder);
    m_mascotHidden = current.mascotHidden;
    m_mascot = new QLineEdit(current.mascotPath, this);
    m_mascot->setPlaceholderText(
        tr("Default: マニフェスト.jpg (pick a file to override)"));
    auto *mascotBrowse = new QPushButton(tr("Browse…"), this);
    connect(mascotBrowse, &QPushButton::clicked, this, [this] {
        const QString p = QFileDialog::getOpenFileName(
            this, tr("Home mascot"), m_mascot->text(),
            tr("Images (*.png *.jpg *.jpeg *.webp *.gif);;All files (*)"));
        if (!p.isEmpty()) {
            m_mascot->setText(p);
            m_mascotHidden = false;
            updateApplyState();
        }
    });
    auto *mascotRemove = new QPushButton(tr("Remove"), this);
    mascotRemove->setToolTip(tr("Hide the mascot, including the default"));
    connect(mascotRemove, &QPushButton::clicked, this, [this] {
        m_mascot->clear();
        m_mascotHidden = true;
        updateApplyState();
    });
    auto *mascotReset = new QPushButton(tr("Reset to default"), this);
    mascotReset->setToolTip(tr("Use the shipped マニフェスト.jpg again"));
    connect(mascotReset, &QPushButton::clicked, this, [this] {
        m_mascot->clear();
        m_mascotHidden = false;
        updateApplyState();
    });
    auto *mascotRow = new QHBoxLayout();
    mascotRow->addWidget(m_mascot, 1);
    mascotRow->addWidget(mascotBrowse);
    mascotRow->addWidget(mascotRemove);
    mascotRow->addWidget(mascotReset);
    appLay->addRow(tr("Home mascot:"), mascotRow);
    m_mascotOpacity = new QSlider(Qt::Horizontal, this);
    m_mascotOpacity->setRange(10, 100);
    m_mascotOpacity->setValue(qRound(current.mascotOpacity * 100.0));
    m_mascotOpacityLabel = new QLabel(this);
    auto *opRow = new QHBoxLayout();
    opRow->addWidget(m_mascotOpacity, 1);
    opRow->addWidget(m_mascotOpacityLabel);
    auto opUpdate = [this](int v) {
        if (m_mascotOpacityLabel)
            m_mascotOpacityLabel->setText(tr("%1%").arg(v));
    };
    opUpdate(m_mascotOpacity->value());
    connect(m_mascotOpacity, &QSlider::valueChanged, this, opUpdate);
    connect(m_mascotOpacity, &QSlider::valueChanged, this,
            [this] { updateApplyState(); });
    appLay->addRow(tr("Mascot opacity:"), opRow);
    m_mascotMargin = new QSpinBox(this);
    m_mascotMargin->setRange(0, 256);
    m_mascotMargin->setSuffix(tr(" px"));
    m_mascotMargin->setValue(current.mascotMargin);
    m_mascotMargin->setToolTip(
        tr("Padding between the mascot and the window corner"));
    connect(m_mascotMargin, QOverload<int>::of(&QSpinBox::valueChanged), this,
            [this] { updateApplyState(); });
    appLay->addRow(tr("Mascot margin:"), m_mascotMargin);
    lookPage->addWidget(app);

    // ── Grid ──
    auto *grid = new QGroupBox(tr("Grid"), this);
    auto *gridLay = new QFormLayout(grid);
    m_aspect = new QComboBox(this);
    const QList<QPair<QString, QVariantList>> ratios = {
        {tr("Square 1:1"), {1, 1}},
        {tr("Standard 4:3"), {4, 3}},
        {tr("Photo 3:2"), {3, 2}},
        {tr("Wide 16:9"), {16, 9}},
        {tr("Portrait 3:4"), {3, 4}},
    };
    int aspectIdx = -1;
    for (int i = 0; i < ratios.size(); ++i) {
        m_aspect->addItem(ratios[i].first, ratios[i].second);
        if (ratios[i].second.value(0).toInt() == current.aspectNum &&
            ratios[i].second.value(1).toInt() == current.aspectDen)
            aspectIdx = i;
    }
    if (aspectIdx < 0) {
        m_customNum = current.aspectNum;
        m_customDen = current.aspectDen;
        m_aspect->addItem(
            tr("Custom %1:%2").arg(m_customNum).arg(m_customDen),
            QVariantList{m_customNum, m_customDen});
        aspectIdx = m_aspect->count() - 1;
    }
    m_aspect->setCurrentIndex(aspectIdx);
    gridLay->addRow(tr("Thumbnail ratio:"), m_aspect);
    m_scrollRows = new QDoubleSpinBox(this);
    m_scrollRows->setRange(0.1, 3.0);
    m_scrollRows->setSingleStep(0.1);
    m_scrollRows->setDecimals(1);
    m_scrollRows->setSuffix(tr(" rows"));
    m_scrollRows->setValue(current.gridScrollRows);
    m_scrollRows->setToolTip(tr("Grid rows per mouse-wheel notch"));
    gridLay->addRow(tr("Scroll speed:"), m_scrollRows);
    lookPage->addWidget(grid);

    // ── Explicit detection ──
    auto *exp = new QGroupBox(tr("Explicit detection"), this);
    auto *expLay = new QVBoxLayout(exp);
    expLay->addWidget(note(tr("Non-explicit posts carrying any of these tags are shown as explicit "
        "(in memory only, files untouched).")));
    m_explicit = new QPlainTextEdit(this);
    m_explicit->setPlainText(current.explicitTags.join(u'\n'));
    m_explicit->setMaximumBlockCount(4000);
    m_explicit->setTabChangesFocus(true);
    m_explicit->setMinimumHeight(160);
    expLay->addWidget(m_explicit, 1);
    auto *expRow = new QHBoxLayout();
    expRow->setContentsMargins(0, 0, 0, 0);
    m_explicitCount = new QLabel(this);
    m_explicitCount->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    auto *expDefaults = new QPushButton(tr("Restore defaults"), this);
    connect(expDefaults, &QPushButton::clicked, this, [this] {
        m_explicit->setPlainText(defaultExplicitTagList().join(u'\n'));
    });
    expRow->addWidget(m_explicitCount, 1);
    expRow->addWidget(expDefaults);
    expLay->addLayout(expRow);
    auto updateExpCount = [this] {
        const int n = normalizeExplicitTags(
                          m_explicit->toPlainText().split(u'\n')).size();
        m_explicitCount->setText(tr("%1 tags (one per line)").arg(n));
    };
    connect(m_explicit, &QPlainTextEdit::textChanged, this, updateExpCount);
    updateExpCount();
    tagsPage->addWidget(exp, 1);

    // ── Playback ──
    auto *play = new QGroupBox(tr("Video playback"), this);
    auto *playLay = new QVBoxLayout(play);
    m_muted = new QCheckBox(tr("Mute videos by default"), this);
    m_loop = new QCheckBox(tr("Loop videos"), this);
    m_muted->setChecked(current.videoMuted);
    m_loop->setChecked(current.videoLoop);
    auto *volRow = new QHBoxLayout();
    volRow->addWidget(new QLabel(tr("Default volume:"), this));
    m_volume = new QSlider(Qt::Horizontal, this);
    m_volume->setRange(0, 100);
    m_volume->setValue(current.volume);
    volRow->addWidget(m_volume, 1);
    playLay->addWidget(m_muted);
    playLay->addWidget(m_loop);
    playLay->addLayout(volRow);
    lookPage->addWidget(play);

    // ── Viewer ──
    auto *view = new QGroupBox(tr("Viewer"), this);
    auto *viewLay = new QFormLayout(view);
    m_maxSide = new QComboBox(this);
    for (int s : {1280, 1920, 2560, 3840})
        m_maxSide->addItem(tr("%1 px longest side").arg(s), s);
    m_maxSide->setCurrentIndex(qMax(0, m_maxSide->findData(current.viewerMaxSide)));
    viewLay->addRow(QStringLiteral("Huge-image downscale:"), m_maxSide);
    lookPage->addWidget(view);

    lay->addStretch(1);
    libraryPage->addStretch(1);
    browsePage->addStretch(1);
    lookPage->addStretch(1);

    auto *buttons = new QDialogButtonBox(this);
    buttons->setStandardButtons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel |
                                QDialogButtonBox::RestoreDefaults |
                                QDialogButtonBox::Apply);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        harvest();
        accept();
    });
    m_applyBtn = buttons->button(QDialogButtonBox::Apply);
    m_applyBtn->setEnabled(false); // nothing changed yet
    m_applyBtn->setToolTip(tr("Apply the current changes"));
    connect(m_applyBtn, &QPushButton::clicked, this, [this] {
        harvest();
        // New baseline: nothing left to apply until the next edit.
        m_initial = m_result;
        updateApplyState();
        emit applyRequested();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons->button(QDialogButtonBox::RestoreDefaults), &QPushButton::clicked,
            this, [this] {
                const AppSettings d;
                m_defaultTab->setCurrentIndex(0);
                if (m_language)
                    m_language->setCurrentIndex(0);
                m_classify->setChecked(d.classifyTags);
                if (m_gridSnap)
                    m_gridSnap->setChecked(d.gridSnap);
                if (m_gridStaggered)
                    m_gridStaggered->setChecked(d.gridStaggered);
                m_favAutoDownload->setChecked(d.favAutoDownload);
                m_favFav->setChecked(d.favAutoFav);
                m_unfavDeletes->setChecked(d.unfavDeletesFile);
                m_deleteUnfavs->setChecked(d.deleteUnfavs);
                m_blacklist->clear();
                if (m_favTagList) {
                    m_favTagList->clear();
                    for (const QString &t : d.favTags)
                        m_favTagList->addItem(t);
                }
                m_fullCache->setChecked(d.fullCache);
                if (m_cacheFavs)
                    m_cacheFavs->setChecked(d.cacheFavs);
                m_cacheLimit->setValue(d.cacheLimitMB);
                m_browseLimit->setCurrentIndex(
                    qMax(0, m_browseLimit->findData(d.browseLimit)));
                if (m_dragMode)
                    m_dragMode->setCurrentIndex(qMax(
                        0, m_dragMode->findData(int(d.dragLinkMode))));
                m_dataDir->setText(effectiveDataDir(d));
                if (m_cacheDir)
                    m_cacheDir->setText(d.cacheDir);
                m_downloadDir->setText(effectiveDownloadDir(d));
                m_ugoiraFormat->setCurrentIndex(
                    qMax(0, m_ugoiraFormat->findData(d.ugoiraFormat)));
                m_ugoiraDelay->setValue(d.ugoiraFrameDelayMs);
                m_tagOverrideEdit->clear();
                m_tagOverrides->clear(); // defaults carry no overrides
                m_pendingFavicons.clear(); // staged icons follow the reset
                updateFaviconPreview();
                if (m_categoryMerges)
                    m_categoryMerges->clear();
                if (m_mergeScope)
                    m_mergeScope->setCurrentIndex(0);
                if (m_customCategories)
                    m_customCategories->clear();
                if (m_tagCustomAssignments)
                    m_tagCustomAssignments->clear();
                if (m_tagCustomCategory)
                    m_tagCustomCategory->clear();
                m_booruEdits = RemoteSource::presetBoorus();
                refreshBooruList();
                for (int i = 0; i < m_defBoorus->count(); ++i) {
                    QListWidgetItem *item = m_defBoorus->item(i);
                    item->setCheckState(
                        d.browseSites.contains(item->data(Qt::UserRole).toString())
                            ? Qt::Checked
                            : Qt::Unchecked);
                }
                m_indexCache->setChecked(d.indexCache);
                switch (d.cacheMode) {
                case CacheMode::None:
                    m_cacheNone->setChecked(true);
                    break;
                case CacheMode::SfwOnly:
                    m_cacheSfw->setChecked(true);
                    break;
                case CacheMode::All:
                default:
                    m_cacheAll->setChecked(true);
                    break;
                }
                m_thumbSize->setCurrentIndex(qMax(0, m_thumbSize->findData(d.thumbLongSide)));
                m_themeMode->setCurrentIndex(0);
                m_sidebarSingleScroll->setChecked(false);
                if (m_plusLocalFolder)
                    m_plusLocalFolder->setChecked(false);
                m_accent = QColor(d.accent);
                setSwatch(m_accent);
                m_stylesheet->clear();
                m_mascot->clear();
                m_mascotHidden = false;
                if (m_mascotOpacity)
                    m_mascotOpacity->setValue(qRound(d.mascotOpacity * 100.0));
                if (m_mascotMargin)
                    m_mascotMargin->setValue(d.mascotMargin);
                m_aspect->setCurrentIndex(0);
                if (m_scrollRows)
                    m_scrollRows->setValue(d.gridScrollRows);
                m_muted->setChecked(d.videoMuted);
                m_loop->setChecked(d.videoLoop);
                m_volume->setValue(d.volume);
                m_maxSide->setCurrentIndex(
                    qMax(0, m_maxSide->findData(d.viewerMaxSide)));
                m_explicit->setPlainText(d.explicitTags.join(u'\n'));
                updateApplyState();
            });
    // Buttons live outside the scroll area so they stay visible.
    outer->addWidget(buttons);
    // ...and wheel over a control only reaches it after a click (focus);
    // number inputs never take it at all (typed focus lingers, so any
    // later hover-wheel would silently rewrite the value). Otherwise the
    // gesture scrolls the page. No more accidental flips.
    // Wheel events often land on inner children or viewports rather than
    // the control itself, so those get the guard too.
    auto *guard = new WheelGuard(this);
    for (QComboBox *cb : findChildren<QComboBox *>()) {
        cb->installEventFilter(guard);
        for (QWidget *ch : cb->findChildren<QWidget *>())
            ch->installEventFilter(guard);
    }
    for (QAbstractSpinBox *sb : findChildren<QAbstractSpinBox *>()) {
        sb->installEventFilter(guard);
        for (QWidget *ch : sb->findChildren<QWidget *>())
            ch->installEventFilter(guard);
    }
    for (QSlider *sl : findChildren<QSlider *>())
        sl->installEventFilter(guard);
    for (QAbstractItemView *v : findChildren<QAbstractItemView *>()) {
        v->installEventFilter(guard);
        v->viewport()->installEventFilter(guard);
    }
    for (QPlainTextEdit *e : findChildren<QPlainTextEdit *>()) {
        e->installEventFilter(guard);
        e->viewport()->installEventFilter(guard);
    }
    // Any edit lights up Apply; harvest comparison greys it again when
    // the widgets match the last applied state.
    auto track = [this] { updateApplyState(); };
    const auto boxes = findChildren<QCheckBox *>();
    for (QCheckBox *b : boxes)
        connect(b, &QCheckBox::toggled, this, track);
    const auto radios = findChildren<QRadioButton *>();
    for (QRadioButton *r : radios)
        connect(r, &QRadioButton::toggled, this, track);
    const auto combos = findChildren<QComboBox *>();
    for (QComboBox *cb : combos) {
        if (cb != m_tagOverrideBucket) // picked bucket alone changes nothing
            connect(cb, QOverload<int>::of(&QComboBox::currentIndexChanged),
                    this, track);
    }
    if (m_cacheLimit)
        connect(m_cacheLimit, QOverload<int>::of(&QSpinBox::valueChanged),
                this, track);
    if (m_scrollRows)
        connect(m_scrollRows, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                this, track);
    if (m_volume)
        connect(m_volume, &QSlider::valueChanged, this, track);
    const auto edits = findChildren<QLineEdit *>();
    for (QLineEdit *e : edits) {
        if (e != m_tagOverrideEdit) // pending override text applies on Add
            connect(e, &QLineEdit::textChanged, this, track);
    }
    const auto plains = findChildren<QPlainTextEdit *>();
    for (QPlainTextEdit *e : plains)
        connect(e, &QPlainTextEdit::textChanged, this, track);
    if (m_defBoorus)
        connect(m_defBoorus, &QListWidget::itemChanged, this, track);
    if (m_tagOverrides) {
        connect(m_tagOverrides->model(), &QAbstractItemModel::rowsInserted,
                this, track);
        connect(m_tagOverrides->model(), &QAbstractItemModel::rowsRemoved,
                this, track);
        connect(m_tagOverrides->model(), &QAbstractItemModel::dataChanged,
                this, track);
    }
    if (m_booruList) {
        connect(m_booruList->model(), &QAbstractItemModel::rowsInserted,
                this, track);
        connect(m_booruList->model(), &QAbstractItemModel::rowsRemoved,
                this, track);
    }
}

