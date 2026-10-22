#include "prefsdlg.h"
#include "cookiegrab.h"
#include "faviconcache.h"
#include "tagtypes.h"

#include <QAbstractItemView>
#include <QApplication>
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
#include <QTimer>
#include <QScrollArea>
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
            // or drag — the page scrolls instead.
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
    resize(700, 780);
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
        // One flat surface: viewport + page share the window background
        // so no inner box edge shows through around the content.
        scroll->setBackgroundRole(QPalette::Window);
        scroll->viewport()->setBackgroundRole(QPalette::Window);
        scroll->viewport()->setAutoFillBackground(false);
        auto *page = new QWidget(this);
        page->setBackgroundRole(QPalette::Window);
        auto *l = new QVBoxLayout(page);
        l->setContentsMargins(0, 0, 8, 8);
        l->setSpacing(8);
        scroll->setWidget(page);
        tabs->addTab(scroll, title);
        return l;
    };
    // General tab keeps the `lay` name so the sections below read on.
    QVBoxLayout *lay = makePage(tr("General"));
    QVBoxLayout *libraryPage = makePage(tr("Library"));
    QVBoxLayout *browsePage = makePage(tr("Browse"));
    QVBoxLayout *tagsPage = makePage(tr("Tags"));
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
    const QList<BooruSite> tabBoorus =
        current.boorus.isEmpty() ? RemoteSource::presetBoorus() : current.boorus;
    for (const BooruSite &s : tabBoorus)
        m_defaultTab->addItem(tr("%1 browse").arg(s.label), s.key);
    m_defaultTab->setCurrentIndex(
        qMax(0, m_defaultTab->findData(current.tabsDefault)));
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
    m_thumbCache = new QCheckBox(tr("Thumbnail disk cache"), this);
    m_indexCache->setChecked(current.indexCache);
    m_thumbCache->setChecked(current.thumbCache);
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
        tr("Cache full-quality images while browsing"), this);
    m_fullCache->setChecked(current.fullCache);
    auto *limitRow = new QHBoxLayout();
    limitRow->addWidget(new QLabel(tr("Cache size limit:"), this));
    m_cacheLimit = new QSpinBox(this);
    m_cacheLimit->setRange(0, 1048576);
    m_cacheLimit->setSingleStep(256);
    m_cacheLimit->setSuffix(tr(" MB (0 = unlimited)"));
    m_cacheLimit->setValue(current.cacheLimitMB);
    limitRow->addWidget(m_cacheLimit, 1);
    auto *sizeRow = new QHBoxLayout();
    sizeRow->addWidget(new QLabel(tr("Thumbnail size:"), this));
    m_thumbSize = new QComboBox(this);
    for (int s : {256, 360, 512, 768})
        m_thumbSize->addItem(tr("%1 px").arg(s), s);
    m_thumbSize->setCurrentIndex(
        qMax(0, m_thumbSize->findData(current.thumbLongSide)));
    sizeRow->addWidget(m_thumbSize);
    sizeRow->addStretch(1);
    auto *clearBtn = new QPushButton(tr("Clear caches…"), this);
    connect(clearBtn, &QPushButton::clicked, this, [] {
        clearCaches();
        QMessageBox::information(
            nullptr, tr("Caches"),
            tr("Scan index and thumbnail caches cleared.\n"
                           "They rebuild on the next scan."));
    });
    sizeRow->addWidget(clearBtn);
    libLay->addWidget(m_indexCache);
    libLay->addWidget(m_thumbCache);
    libLay->addWidget(m_fullCache);
    libLay->addWidget(m_classify);
    libLay->addLayout(classifyRow);
    libLay->addLayout(limitRow);
    libLay->addWidget(note(tr("Untyped tags (tagType \"none\", Gelbooru blobs) get their category "
        "from public tag APIs. Results cache forever; misses stay general.")));
    libLay->addLayout(sizeRow);
    libLay->addWidget(note(tr("Cache toggles take effect on the next scan.")));
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
    tagsPage->addWidget(types);

    // ── Categories per booru type: untick buckets a taxonomy lacks ──
    auto *cats = new QGroupBox(tr("Categories per booru type"), this);
    auto *catsLay = new QVBoxLayout(cats);
    catsLay->addWidget(note(tr(
        "Unticked buckets fold into general for that booru type (meme hosts "
        "and other non-anime taxonomies). Safebooru follows the Gelbooru "
        "row; general itself is always on.")));
    auto *catGrid = new QGridLayout();
    const QList<QPair<BooruEngine, QString>> engines = {
        {BooruEngine::Danbooru, RemotePosts::engineLabel(BooruEngine::Danbooru)},
        {BooruEngine::Gelbooru, RemotePosts::engineLabel(BooruEngine::Gelbooru)},
        {BooruEngine::Moebooru, RemotePosts::engineLabel(BooruEngine::Moebooru)},
        {BooruEngine::Szurubooru, RemotePosts::engineLabel(BooruEngine::Szurubooru)},
        {BooruEngine::Shimmie, RemotePosts::engineLabel(BooruEngine::Shimmie)},
        {BooruEngine::Hybooru, RemotePosts::engineLabel(BooruEngine::Hybooru)},
        {BooruEngine::Nuuru, RemotePosts::engineLabel(BooruEngine::Nuuru)},
        {BooruEngine::Hiddenbooru,
         RemotePosts::engineLabel(BooruEngine::Hiddenbooru)},
    };
    const QList<QPair<QString, QString>> buckets = {
        {QStringLiteral("artist"), tr("Artist")},
        {QStringLiteral("character"), tr("Character")},
        {QStringLiteral("copyright"), tr("Copyright")},
        {QStringLiteral("meta"), tr("Meta")},
    };
    int c = 1;
    for (const auto &[bucketKey, bucketLabel] : buckets) {
        Q_UNUSED(bucketKey);
        auto *head = new QLabel(bucketLabel, this);
        head->setAlignment(Qt::AlignCenter);
        catGrid->addWidget(head, 0, c++);
    }
    int r = 1;
    for (const auto &[engine, engineLabel] : engines) {
        catGrid->addWidget(new QLabel(engineLabel, this), r, 0);
        c = 1;
        for (const auto &[bucketKey, bucketLabel] : buckets) {
            Q_UNUSED(bucketLabel);
            auto *cb = new QCheckBox(this);
            cb->setChecked(!current.disabledBuckets.contains(
                RemotePosts::engineKey(engine) + u':' + bucketKey));
            cb->setToolTip(tr("%1 tags on %2").arg(bucketKey, engineLabel));
            catGrid->addWidget(cb, r, c++);
            m_bucketToggles.insert(
                RemotePosts::engineKey(engine) + u':' + bucketKey, cb);
        }
        ++r;
    }
    catsLay->addLayout(catGrid);
    tagsPage->addWidget(cats);

    // ── Browse ──
    auto *browse = new QGroupBox(tr("Browse & downloads"), this);
    auto *browseLay = new QFormLayout(browse);
    m_browseLimit = new QComboBox(this);
    for (int n : {20, 40, 60, 100})
        m_browseLimit->addItem(tr("%1 per page").arg(n), n);
    m_browseLimit->setCurrentIndex(
        qMax(0, m_browseLimit->findData(current.browseLimit)));
    browseLay->addRow(tr("Page size:"), m_browseLimit);
    m_defBoorus = new QListWidget(this);
    m_defBoorus->setMinimumHeight(110);
    for (const BooruSite &s : tabBoorus) {
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
        "tag_types.db). Changing it starts fresh there — move "
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
                                "socks5h — nothing leaks to local DNS. A per-booru "
                                "proxy overrides this one; \"direct\" forces that "
                                "booru direct. Empty = direct. The embedded "
                                "browser (capture, PoW walls, onion sites) "
                                "reads it at startup — restart after changing it."));
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
    accBtnRow->addWidget(booruAdd);
    accBtnRow->addWidget(booruDel);
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
    auto *accForm = new QFormLayout(accFormBox);
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
    m_booruEngine->addItem(RemotePosts::engineLabel(BooruEngine::Hiddenbooru),
                           RemotePosts::engineKey(BooruEngine::Hiddenbooru));
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
        auto *typeRow = new QHBoxLayout(typeBox);
        typeRow->setContentsMargins(0, 0, 0, 0);
        m_booruEngine->setSizePolicy(QSizePolicy::Expanding,
                                     QSizePolicy::Fixed);
        typeRow->addWidget(m_booruEngine, 1);
        auto *detectBtn = new QPushButton(tr("Detect"), typeBox);
        detectBtn->setToolTip(
            tr("Probe the instance URL and select the matching engine"));
        typeRow->addWidget(detectBtn);
        connect(detectBtn, &QPushButton::clicked, this,
                [this, detectBtn] { detectBooruEngine(detectBtn); });
    }
    accForm->addRow(tr("Type:"), typeBox);
    accForm->addRow(tr("Tags:"), m_booruNoClassify);
    accForm->addRow(tr("Blacklist:"), m_booruLocalBlacklist);
    accForm->addRow(tr("Default tags:"), m_booruTags);
    auto *catBox = new QWidget(this);
    {
        auto *catRow = new QHBoxLayout(catBox);
        catRow->setContentsMargins(0, 0, 0, 0);
        const QList<QPair<QString, QString>> buckets = {
            {QStringLiteral("artist"), tr("Artist")},
            {QStringLiteral("character"), tr("Character")},
            {QStringLiteral("copyright"), tr("Copyright")},
            {QStringLiteral("meta"), tr("Meta")},
        };
        for (const auto &[key, label] : buckets) {
            auto *cb = new QCheckBox(label, this);
            cb->setChecked(true);
            cb->setToolTip(tr("Untick buckets this host's taxonomy lacks; "
                              "they fold into general."));
            m_booruBuckets.insert(key, cb);
            catRow->addWidget(cb);
        }
        catRow->addStretch(1);
    }
    accForm->addRow(tr("Categories:"), catBox);
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
                                 "logged in) — no puzzle to solve."));
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
                        tr("✗ %1: still challenged — open Capture in "
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
                    tr("✓ %1: %2 cookie(s) refreshed — Apply to save.")
                        .arg(label).arg(fresh.size()));
            });
    });
    auto *cookieRow = new QHBoxLayout();
    cookieRow->addWidget(m_booruCookieLabel, 1);
    cookieRow->addWidget(cookieEdit);
    cookieRow->addWidget(cookieGrab);
    cookieRow->addWidget(cookieRefresh);
    accForm->addRow(tr("Cookies:"), cookieRow);
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
            this, tr("Custom favicon — %1").arg(e->label),
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
    accForm->addRow(QStringLiteral(""), testBtn);
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
    m_themeMode->addItem(tr("Built-in dark (Fusion, recommended)"),
                         QStringLiteral("builtin"));
    m_themeMode->addItem(tr("System native"), QStringLiteral("native"));
    m_themeMode->setCurrentIndex(current.themeMode == QStringLiteral("native") ? 1 : 0);
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
    m_mascot = new QLineEdit(current.mascotPath, this);
    m_mascot->setPlaceholderText(tr("Optional home-page mascot image"));
    auto *mascotBrowse = new QPushButton(tr("Browse…"), this);
    connect(mascotBrowse, &QPushButton::clicked, this, [this] {
        const QString p = QFileDialog::getOpenFileName(
            this, tr("Home mascot"), m_mascot->text(),
            tr("Images (*.png *.jpg *.jpeg *.webp *.gif);;All files (*)"));
        if (!p.isEmpty())
            m_mascot->setText(p);
    });
    auto *mascotClear = new QPushButton(tr("Clear"), this);
    connect(mascotClear, &QPushButton::clicked, m_mascot, &QLineEdit::clear);
    auto *mascotRow = new QHBoxLayout();
    mascotRow->addWidget(m_mascot, 1);
    mascotRow->addWidget(mascotBrowse);
    mascotRow->addWidget(mascotClear);
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
                m_favAutoDownload->setChecked(d.favAutoDownload);
                m_favFav->setChecked(d.favAutoFav);
                m_unfavDeletes->setChecked(d.unfavDeletesFile);
                m_deleteUnfavs->setChecked(d.deleteUnfavs);
                m_blacklist->clear();
                m_fullCache->setChecked(d.fullCache);
                m_cacheLimit->setValue(d.cacheLimitMB);
                m_browseLimit->setCurrentIndex(
                    qMax(0, m_browseLimit->findData(d.browseLimit)));
                m_dataDir->setText(effectiveDataDir(d));
                m_downloadDir->setText(effectiveDownloadDir(d));
                m_ugoiraFormat->setCurrentIndex(
                    qMax(0, m_ugoiraFormat->findData(d.ugoiraFormat)));
                m_ugoiraDelay->setValue(d.ugoiraFrameDelayMs);
                m_tagOverrideEdit->clear();
                m_tagOverrides->clear(); // defaults carry no overrides
                m_pendingFavicons.clear(); // staged icons follow the reset
                updateFaviconPreview();
                for (QCheckBox *cb : std::as_const(m_bucketToggles))
                    cb->setChecked(true); // defaults enable every bucket
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
                m_thumbCache->setChecked(d.thumbCache);
                m_thumbSize->setCurrentIndex(qMax(0, m_thumbSize->findData(d.thumbLongSide)));
                m_themeMode->setCurrentIndex(0);
                m_accent = QColor(d.accent);
                setSwatch(m_accent);
                m_stylesheet->clear();
                m_mascot->clear();
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

void PrefsDialog::setSwatch(const QColor &c) {
    m_accentBtn->setStyleSheet(
        QStringLiteral("background:%1;border:1px solid #3a3f55;border-radius:4px;"
                       "padding:4px 18px;color:%2;font-weight:bold;")
            .arg(c.name(),
                 c.lightness() > 128 ? QStringLiteral("black")
                                     : QStringLiteral("white")));
    m_accentBtn->setText(c.name());
}

void PrefsDialog::setBooruSwatch() {
    m_booruColorBtn->setStyleSheet(
        QStringLiteral("background:%1;border:1px solid #3a3f55;border-radius:4px;"
                       "padding:4px 18px;color:%2;font-weight:bold;")
            .arg(m_booruColor.name(),
                 m_booruColor.lightness() > 128 ? QStringLiteral("black")
                                                : QStringLiteral("white")));
    m_booruColorBtn->setText(m_booruColor.name());
}

namespace {
// Blocking engine probe (worker thread): fetch shape-revealing endpoints,
// first match wins. -1 when nothing matches (walled/gone/unknown).
int detectEngineFor(const QString &base) {
    QNetworkAccessManager nam;
    auto get = [&](const QString &url) -> QByteArray {
        QNetworkRequest req{QUrl(url)};
        req.setTransferTimeout(10000);
        req.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("box/0.1 (engine detect)"));
        req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
        QNetworkReply *reply = nam.get(req);
        QEventLoop loop;
        QTimer timer;
        timer.setSingleShot(true);
        QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
        QObject::connect(reply, &QNetworkReply::finished, &loop,
                         &QEventLoop::quit);
        timer.start(12000);
        loop.exec();
        timer.stop();
        QByteArray body;
        if (reply->error() == QNetworkReply::NoError)
            body = reply->readAll();
        reply->deleteLater();
        return body;
    };
    auto firstObj = [](const QByteArray &body) -> QJsonObject {
        QJsonParseError err;
        const QJsonDocument doc = QJsonDocument::fromJson(body, &err);
        if (err.error != QJsonParseError::NoError || !doc.isArray() ||
            doc.array().isEmpty())
            return {};
        const QJsonValue v = doc.array().first();
        return v.isObject() ? v.toObject() : QJsonObject();
    };
    // Shimmie: XML only (danbooru_api ext).
    {
        const QByteArray xml =
            get(base + QStringLiteral("/api/danbooru/post/index.xml?limit=1"));
        if (xml.trimmed().startsWith('<') && xml.contains("<posts"))
            return int(BooruEngine::Shimmie);
    }
    // Szurubooru: version endpoint, else results-shaped listing.
    {
        QJsonParseError err;
        const QJsonDocument info = QJsonDocument::fromJson(
            get(base + QStringLiteral("/api/info")), &err);
        if (err.error == QJsonParseError::NoError && info.isObject() &&
            (info.object().contains(QStringLiteral("serverTime")) ||
             info.object().contains(QStringLiteral("version"))))
            return int(BooruEngine::Szurubooru);
        const QJsonDocument list = QJsonDocument::fromJson(
            get(base + QStringLiteral("/api/posts/?limit=1")), &err);
        if (err.error == QJsonParseError::NoError && list.isObject() &&
            list.object().value(QStringLiteral("results")).isArray())
            return int(BooruEngine::Szurubooru);
    }
    // Danbooru: per-bucket tag_string_* fields.
    {
        const QJsonObject o =
            firstObj(get(base + QStringLiteral("/posts.json?limit=1")));
        if (!o.isEmpty() &&
            (o.contains(QStringLiteral("tag_string_artist")) ||
             o.contains(QStringLiteral("tag_string_general")) ||
             o.contains(QStringLiteral("tag_string_copyright"))))
            return int(BooruEngine::Danbooru);
    }
    // Moebooru: flat "tags" string (epoch dates where present).
    // Gelbooru never serves /post.json, so anything tags-shaped here
    // is moebooru-family.
    {
        const QJsonObject o =
            firstObj(get(base + QStringLiteral("/post.json?limit=1")));
        if (!o.isEmpty() && o.value(QStringLiteral("tags")).isString())
            return int(BooruEngine::Moebooru);
    }
    // Gelbooru dapi family: {"post": [...]} or a bare tags array.
    {
        const QByteArray body = get(
            base +
            QStringLiteral("/index.php?page=dapi&s=post&q=index&limit=1&json=1"));
        QJsonParseError err;
        const QJsonDocument doc = QJsonDocument::fromJson(body, &err);
        if (err.error == QJsonParseError::NoError) {
            if (doc.isObject() &&
                doc.object().value(QStringLiteral("post")).isArray())
                return int(BooruEngine::Gelbooru);
            if (doc.isArray() && !doc.array().isEmpty()) {
                const QJsonValue v = doc.array().first();
                if (v.isObject() &&
                    v.toObject().value(QStringLiteral("tags")).isString())
                    return int(BooruEngine::Gelbooru);
            }
        }
    }
    return -1;
}
} // namespace

void PrefsDialog::detectBooruEngine(QPushButton *detectBtn) {
    saveBooruEditor();
    BooruSite *e = currentBooruEntry();
    if (!e || e->baseUrl.isEmpty()) {
        m_accStatus->setText(tr("✗ set an instance URL first"));
        return;
    }
    detectBtn->setEnabled(false);
    m_accStatus->setText(tr("Detecting engine…"));
    const QString base = e->baseUrl;
    QFutureWatcher<int> *watcher = new QFutureWatcher<int>(this);
    connect(watcher, &QFutureWatcher<int>::finished, this,
            [this, watcher, detectBtn] {
                const int found = watcher->result();
                detectBtn->setEnabled(true);
                watcher->deleteLater();
                if (found < 0) {
                    m_accStatus->setText(
                        tr("✗ engine not recognized — pick it manually"));
                    return;
                }
                const BooruEngine engine = static_cast<BooruEngine>(found);
                m_booruEngine->setCurrentIndex(qMax(
                    0, m_booruEngine->findData(
                           RemotePosts::engineKey(engine))));
                saveBooruEditor();
                updateApplyState();
                m_accStatus->setText(
                    tr("✓ detected: %1")
                        .arg(RemotePosts::engineLabel(engine)));
            });
    watcher->setFuture(QtConcurrent::run([base] {
        return detectEngineFor(base);
    }));
}

void PrefsDialog::testSite(const BooruSite &probed) {
    if (probed.baseUrl.isEmpty()) {
        m_accStatus->setText(
            tr("✗ %1 needs an instance URL first").arg(probed.label));
        return;
    }
    m_accStatus->setText(tr("Testing %1…").arg(probed.label));
    m_accStatus->setCursor(Qt::ArrowCursor);
    m_accStatus->setToolTip(QString());
    m_lastProbeValid = false;
    const SiteCreds creds{probed.user, probed.secret};
    // Probing blocks with its own loop; run it off the UI thread so the
    // dialog stays alive, then report back.
    const BooruSite site = probed;
    qRegisterMetaType<RemoteSource::CredentialProbe>();
    QFutureWatcher<RemoteSource::CredentialProbe> *watcher =
        new QFutureWatcher<RemoteSource::CredentialProbe>(this);
    connect(watcher, &QFutureWatcher<RemoteSource::CredentialProbe>::finished,
            this, [this, watcher] {
                m_lastProbe = watcher->result();
                m_lastProbeValid = true;
                m_accStatus->setCursor(Qt::PointingHandCursor);
                m_accStatus->setToolTip(
                    tr("Click to see the full server response"));
                const QString label = m_lastProbe.siteLabel;
                const bool ok = m_lastProbe.ok;
                QString msg = m_lastProbe.message;
                // Server error bodies can be huge unbroken blobs: cap the
                // line so even unwrapped it cannot stretch the dialog.
                static constexpr int kMaxMsg = 240;
                if (msg.size() > kMaxMsg)
                    msg = msg.left(kMaxMsg) + QStringLiteral("…");
                m_accStatus->setText(
                    tr("%1 %2: %3").arg(ok ? QStringLiteral("✓")
                                           : QStringLiteral("✗"),
                                      label, msg));
                watcher->deleteLater();
            });
    watcher->setFuture(QtConcurrent::run([site, creds] {
        return RemoteSource::probeCredentials(site, creds);
    }));
}

bool PrefsDialog::eventFilter(QObject *o, QEvent *e) {
    if (o == m_accStatus && e->type() == QEvent::MouseButtonPress &&
        m_lastProbeValid) {
        showProbeDetails();
        return true;
    }
    return QDialog::eventFilter(o, e);
}

void PrefsDialog::showProbeDetails() {
    if (!m_lastProbeValid)
        return;
    QString cookies =
        m_lastProbe.sentCookieNames.isEmpty()
            ? tr("(none)")
            : m_lastProbe.sentCookieNames.join(QStringLiteral(", "));
    RemotePosts::showProbeDialog(
        this, tr("Server response — %1").arg(m_lastProbe.siteLabel),
        RemotePosts::probeReportText(
            RemotePosts::redactSecrets(
                m_lastProbe.url.toString(QUrl::FullyEncoded)),
            m_lastProbe.http, m_lastProbe.body) +
            tr("\nCookies sent: %1").arg(cookies));
}

void PrefsDialog::commitBooruEditor(const QString &key) {
    if (key.isEmpty() || m_loadingBooru)
        return;
    for (BooruSite &b : m_booruEdits) {
        if (b.key != key)
            continue;
        b.label = m_booruName->text().trimmed();
        if (b.label.isEmpty())
            b.label = b.key;
        b.shortName = m_booruShort->text().trimmed();
        if (b.shortName.isEmpty())
            b.shortName = RemotePosts::shortFor(b.label);
        b.badgeColor = m_booruColor.name();
        QString base = m_booruBase->text().trimmed();
        while (base.endsWith(u'/'))
            base.chop(1);
        const QString oldHost = QUrl(b.baseUrl).host().toLower();
        const QString newHost = QUrl(base).host().toLower();
        b.baseUrl = base;
        b.engine = RemotePosts::engineForKey(m_booruEngine->currentData().toString());
        b.user = m_booruUser->text().trimmed();
        b.secret = m_booruSecret->text();
        b.skipAutoClassify = m_booruNoClassify->isChecked();
        b.localBlacklist = m_booruLocalBlacklist->isChecked();
        b.defaultTags = m_booruTags->text().trimmed();
        b.proxy = m_booruProxy->text().trimmed();
        // Fresh entries still wearing the generic blue inherit the new
        // host's house color (explicit picks and existing entries are
        // untouched: this only fires while the color is still default).
        if (b.badgeColor.compare(QStringLiteral("#3a7ec2"),
                                 Qt::CaseInsensitive) == 0 &&
            QRegularExpression(QStringLiteral("^newbooru\\d*$"))
                .match(b.key)
                .hasMatch())
            b.badgeColor = RemotePosts::defaultBadgeColor(b.baseUrl, b.engine);
        // Unchecked buckets fold into general for this host only.
        b.disabledBuckets.clear();
        for (auto it = m_booruBuckets.begin(); it != m_booruBuckets.end();
             ++it) {
            if (!it.value()->isChecked())
                b.disabledBuckets.insert(it.key());
        }
        // Seed per-host default cookies once (quirks table: gelbooru.com
        // wants fringeBenefits=yup for restricted content). They land in
        // the stored list, so they stay visible - and removable - in the
        // cookie manager. Keyed on the host transition so a deliberate
        // removal is never re-added behind your back.
        if (oldHost != newHost)
            b.cookies << RemotePosts::seedQuirkCookies(b.cookies, newHost);
        // Fresh "newbooru" entries get a descriptive key the first time
        // they are named or pointed at a host ("Rule34" -> rule34,
        // https://rule34.xxx -> rule34). After that the key is frozen:
        // saved tabs and defaults reference it.
        if (QRegularExpression(QStringLiteral("^newbooru\\d*$"))
                .match(b.key)
                .hasMatch()) {
            QSet<QString> taken;
            for (const BooruSite &o : m_booruEdits) {
                if (o.key != b.key)
                    taken.insert(o.key);
            }
            QString candidate;
            if (b.label.compare(QStringLiteral("New booru"),
                               Qt::CaseInsensitive) != 0)
                candidate = RemotePosts::uniqueBooruKey(b.label, taken);
            else if (!newHost.isEmpty() && newHost != QStringLiteral("www"))
                candidate = RemotePosts::uniqueBooruKey(
                    newHost.split(u'.').first(), taken);
            else if (!newHost.isEmpty())
                candidate = RemotePosts::uniqueBooruKey(newHost, taken);
            if (!candidate.isEmpty() && candidate != b.key) {
                const QString oldKey = b.key;
                b.key = candidate;
                // A staged favicon follows the rename.
                if (m_pendingFavicons.contains(oldKey))
                    m_pendingFavicons.insert(
                        candidate, m_pendingFavicons.take(oldKey));
                // Keep the list + defaults picker pointing at the entry.
                for (int i = 0; i < m_booruList->count(); ++i) {
                    QListWidgetItem *item = m_booruList->item(i);
                    if (item->data(Qt::UserRole).toString() == oldKey) {
                        item->setData(Qt::UserRole, candidate);
                        item->setText(b.label);
                    }
                }
                if (m_defBoorus) {
                    for (int i = 0; i < m_defBoorus->count(); ++i) {
                        QListWidgetItem *item = m_defBoorus->item(i);
                        if (item->data(Qt::UserRole).toString() == oldKey)
                            item->setData(Qt::UserRole, candidate);
                    }
                }
            }
        }
        // Live list title: the left pane follows every keystroke, not
        // just the one-time key rename above (which is why the name
        // used to freeze after its first letter).
        for (int i = 0; i < m_booruList->count(); ++i) {
            QListWidgetItem *item = m_booruList->item(i);
            if (item->data(Qt::UserRole).toString() == b.key) {
                item->setText(b.label);
                break;
            }
        }
        return;
    }
}

void PrefsDialog::saveBooruEditor() {
    BooruSite *e = currentBooruEntry();
    if (e)
        commitBooruEditor(e->key);
}

BooruSite *PrefsDialog::currentBooruEntry() {
    const int row = m_booruList ? m_booruList->currentRow() : -1;
    if (row < 0 || !m_booruList)
        return nullptr;
    const QListWidgetItem *item = m_booruList->item(row);
    const QString key = item ? item->data(Qt::UserRole).toString() : QString();
    if (key.isEmpty())
        return nullptr;
    for (BooruSite &b : m_booruEdits) {
        if (b.key == key)
            return &b;
    }
    return nullptr;
}

void PrefsDialog::updateCookieLabel() {
    if (!m_booruCookieLabel)
        return;
    const BooruSite *e = currentBooruEntry();
    const int n = e ? e->cookies.size() : 0;
    m_booruCookieLabel->setText(
        n == 0 ? tr("none") : tr("%1 set").arg(n));
}

QString PrefsDialog::customFaviconDir() const {
    QString dataDir = m_dataDir ? m_dataDir->text().trimmed() : QString();
    if (dataDir.isEmpty())
        dataDir = defaultDataDir();
    return dataDir + QStringLiteral("/favicons");
}

void PrefsDialog::updateFaviconPreview() {
    if (!m_booruFavicon)
        return;
    const BooruSite *e = currentBooruEntry();
    QPixmap pm;
    QString tip;
    if (e) {
        // Staged choice wins, then the saved custom file, then the
        // auto-fetched icon (if the cache already holds one).
        QString src = m_pendingFavicons.value(e->key);
        if (src.isEmpty() && !m_pendingFavicons.contains(e->key))
            src = customFaviconDir() + u'/' + e->key +
                  QStringLiteral(".png");
        if (!src.isEmpty()) {
            pm = QIcon(src).pixmap(16, 16);
            tip = src;
        }
        if (pm.isNull() && m_favicons) {
            const QIcon autoIcon = m_favicons->iconFor(e->key, e->baseUrl);
            if (!autoIcon.isNull()) {
                pm = autoIcon.pixmap(16, 16);
                tip = tr("Auto-fetched");
            }
        }
    }
    if (pm.isNull()) {
        m_booruFavicon->setText(tr("—"));
        m_booruFavicon->setToolTip(tr("No icon yet"));
    } else {
        m_booruFavicon->setText(QString());
        m_booruFavicon->setPixmap(pm);
        m_booruFavicon->setToolTip(tip);
    }
}

void PrefsDialog::loadBooruEditor(int row) {
    const BooruSite *found = nullptr;
    if (row >= 0) {
        const QListWidgetItem *item = m_booruList->item(row);
        const QString key = item ? item->data(Qt::UserRole).toString() : QString();
        for (const BooruSite &b : m_booruEdits) {
            if (b.key == key) {
                found = &b;
                break;
            }
        }
    }
    const bool on = found != nullptr;
    // Disabled controls hide entirely instead of sitting greyed-out.
    m_booruFormBox->setVisible(on);
    m_booruEmpty->setVisible(!on);
    // Guarded: these programmatic fills emit textChanged, which must not
    // commit half-loaded widgets back into the entry (see m_loadingBooru).
    m_loadingBooru = true;
    if (!found) {
        m_booruName->clear();
        m_booruBase->clear();
        m_booruUser->clear();
        m_booruSecret->clear();
        m_booruShort->clear();
        m_booruNoClassify->setChecked(false);
        m_booruLocalBlacklist->setChecked(false);
        for (auto it = m_booruBuckets.begin(); it != m_booruBuckets.end();
             ++it)
            it.value()->setChecked(true);
        m_booruTags->clear();
        m_booruProxy->clear();
        m_booruColor = QColor(QStringLiteral("#3a7ec2"));
        setBooruSwatch();
        updateCookieLabel();
        updateFaviconPreview();
        m_loadingBooru = false;
        return;
    }
    m_booruName->setText(found->label);
    m_booruShort->setText(found->shortName);
    m_booruColor = QColor(found->badgeColor);
    if (!m_booruColor.isValid())
        m_booruColor = QColor(QStringLiteral("#3a7ec2"));
    setBooruSwatch();
    m_booruBase->setText(found->baseUrl);
    m_booruEngine->setCurrentIndex(
        qMax(0, m_booruEngine->findData(RemotePosts::engineKey(found->engine))));
    m_booruUser->setText(found->user);
    m_booruSecret->setText(found->secret);
    m_booruNoClassify->setChecked(found->skipAutoClassify);
    m_booruLocalBlacklist->setChecked(found->localBlacklist);
    m_booruTags->setText(found->defaultTags);
    m_booruProxy->setText(found->proxy);
    for (auto it = m_booruBuckets.begin(); it != m_booruBuckets.end();
         ++it)
        it.value()->setChecked(!found->disabledBuckets.contains(it.key()));
    updateCookieLabel();
    updateFaviconPreview();
    m_loadingBooru = false;
}

void PrefsDialog::refreshBooruList() {
    m_booruList->blockSignals(true);
    m_booruList->clear();
    for (const BooruSite &b : m_booruEdits) {
        auto *item = new QListWidgetItem(b.label, m_booruList);
        item->setData(Qt::UserRole, b.key);
    }
    m_booruList->blockSignals(false);
    if (m_booruList->count() > 0 && m_booruList->currentRow() < 0)
        m_booruList->setCurrentRow(0);
    else
        loadBooruEditor(m_booruList->currentRow());
}

void PrefsDialog::refreshBooruListLabels() {
    for (int i = 0; i < m_booruList->count(); ++i) {
        QListWidgetItem *item = m_booruList->item(i);
        const QString key = item->data(Qt::UserRole).toString();
        for (const BooruSite &b : m_booruEdits) {
            if (b.key == key) {
                item->setText(b.label);
                break;
            }
        }
    }
}

void PrefsDialog::harvest() {
    harvestInto(m_result);
    commitPendingFavicons();
}

// Custom favicons: convert staged choices to
// <dataDir>/favicons/<key>.png, drop files for removed entries (and
// staged removals). Runs only on real Apply/OK — harvestInto doubles
// as the Apply-state comparison and must never touch files.
void PrefsDialog::commitPendingFavicons() {
    if (m_pendingFavicons.isEmpty())
        return;
    const QString favDir = effectiveDataDir(m_result) + QStringLiteral("/favicons");
    QSet<QString> live;
    for (const BooruSite &b : m_result.boorus)
        live.insert(b.key);
    QDir().mkpath(favDir);
    for (auto it = m_pendingFavicons.begin();
         it != m_pendingFavicons.end(); ++it) {
        const QString dest = favDir + u'/' + it.key() +
                             QStringLiteral(".png");
        if (it.value().isEmpty()) {
            QFile::remove(dest);
            continue;
        }
        QImage img(it.value());
        if (img.isNull()) {
            m_accStatus->setText(
                tr("✗ favicon image unreadable: %1").arg(it.value()));
            continue;
        }
        if (!img.scaled(32, 32, Qt::KeepAspectRatio,
                        Qt::SmoothTransformation)
                 .save(dest, "PNG")) {
            m_accStatus->setText(
                tr("✗ cannot write favicon: %1").arg(dest));
        }
    }
    m_pendingFavicons.clear();
    QDir dir(favDir);
    for (const QString &f :
         dir.entryList({QStringLiteral("*.png")}, QDir::Files)) {
        if (!live.contains(f.left(f.size() - 4)))
            QFile::remove(favDir + u'/' + f);
    }
    updateFaviconPreview();
}

void PrefsDialog::updateApplyState() {
    if (!m_applyBtn)
        return;
    AppSettings cur = m_result;
    harvestInto(cur);
    // Staged favicons live outside AppSettings: any pending choice or
    // removal is itself an unapplied change.
    m_applyBtn->setEnabled(cur != m_initial || !m_pendingFavicons.isEmpty());
}

void PrefsDialog::harvestInto(AppSettings &out) {
    out = m_result; // carry history/session/geometry: not edited here
    out.tabsDefault = m_defaultTab->currentData().toString();
    out.language = m_language ? m_language->currentData().toString() : out.language;
    out.classifyTags = m_classify->isChecked();
    out.favAutoDownload = m_favAutoDownload->isChecked();
    out.favAutoFav = m_favFav->isChecked();
    out.unfavDeletesFile = m_unfavDeletes->isChecked();
    out.deleteUnfavs = m_deleteUnfavs->isChecked();
    out.blacklist =
        normalizeExplicitTags(m_blacklist->toPlainText().split(u'\n'));
    out.browseLimit = m_browseLimit->currentData().toInt();
    out.ugoiraFormat = m_ugoiraFormat->currentData().toString();
    if (out.ugoiraFormat != QStringLiteral("webp") &&
        out.ugoiraFormat != QStringLiteral("jxl") &&
        out.ugoiraFormat != QStringLiteral("zip"))
        out.ugoiraFormat = QStringLiteral("webp");
    out.ugoiraFrameDelayMs = m_ugoiraDelay->value();
    out.browseSites.clear();
    if (m_defBoorus) {
        for (int i = 0; i < m_defBoorus->count(); ++i) {
            const QListWidgetItem *item = m_defBoorus->item(i);
            if (item->checkState() == Qt::Checked)
                out.browseSites << item->data(Qt::UserRole).toString();
        }
    }
    if (out.browseSites.isEmpty()) {
        // Refuse an empty default set: fall back to all configured boorus.
        for (const BooruSite &b : out.boorus)
            out.browseSites << b.key;
    }
    out.dataDir = m_dataDir->text().trimmed();
    while (out.dataDir.endsWith(u'/') && out.dataDir.size() > 1)
        out.dataDir.chop(1);
    if (out.dataDir == defaultDataDir())
        out.dataDir.clear(); // default stays implicit
    // The download default follows the data dir: resolve it after dataDir.
    out.downloadDir = m_downloadDir->text().trimmed();
    while (out.downloadDir.endsWith(u'/') && out.downloadDir.size() > 1)
        out.downloadDir.chop(1);
    if (out.downloadDir == effectiveDownloadDir(out))
        out.downloadDir.clear(); // default stays implicit
    out.globalProxy = m_globalProxy ? m_globalProxy->text().trimmed()
                                    : QString();
    out.wallAutoPopup =
        m_wallAutoPopup ? m_wallAutoPopup->isChecked() : out.wallAutoPopup;
    saveBooruEditor();
    out.boorus = m_booruEdits;
    out.fullCache = m_fullCache->isChecked();
    out.cacheLimitMB = m_cacheLimit->value();
    out.indexCache = m_indexCache->isChecked();
    out.thumbCache = m_thumbCache->isChecked();
    out.thumbLongSide = m_thumbSize->currentData().toInt();
    out.themeMode = m_themeMode->currentData().toString();
    out.accent = m_accent.name();
    out.stylesheet = m_stylesheet->text().trimmed();
    out.mascotPath = m_mascot ? m_mascot->text().trimmed() : QString();
    out.mascotOpacity = m_mascotOpacity ? m_mascotOpacity->value() / 100.0
                                        : out.mascotOpacity;
    out.mascotMargin = m_mascotMargin ? m_mascotMargin->value() : out.mascotMargin;
    const QVariantList r = m_aspect->currentData().toList();
    if (r.size() == 2) {
        out.aspectNum = qMax(1, r.value(0).toInt());
        out.aspectDen = qMax(1, r.value(1).toInt());
    }
    out.gridScrollRows = m_scrollRows ? m_scrollRows->value() : out.gridScrollRows;
    out.videoMuted = m_muted->isChecked();
    out.videoLoop = m_loop->isChecked();
    out.volume = m_volume->value();
    out.viewerMaxSide = m_maxSide->currentData().toInt();
    out.explicitTags =
        normalizeExplicitTags(m_explicit->toPlainText().split(u'\n'));
    out.manualTagTypes.clear();
    if (m_tagOverrides) {
        for (int i = 0; i < m_tagOverrides->count(); ++i) {
            const QListWidgetItem *item = m_tagOverrides->item(i);
            const QString tag = item->data(Qt::UserRole).toString();
            const QString bucket = item->data(Qt::UserRole + 1).toString();
            if (!tag.isEmpty() && TagTypes::isValidBucket(bucket))
                out.manualTagTypes.insert(tag, bucket);
        }
    }
    out.customBucketMap.clear();
    if (m_customBuckets) {
        for (int i = 0; i < m_customBuckets->count(); ++i) {
            const QListWidgetItem *item = m_customBuckets->item(i);
            const QString cat = item->data(Qt::UserRole).toString();
            const QString bucket = item->data(Qt::UserRole + 1).toString();
            if (!cat.isEmpty() && TagTypes::isValidBucket(bucket))
                out.customBucketMap.insert(cat, bucket);
        }
    }
    out.disabledBuckets.clear();
    for (auto it = m_bucketToggles.begin(); it != m_bucketToggles.end(); ++it) {
        if (!it.value()->isChecked())
            out.disabledBuckets.insert(it.key());
    }
}
