#include "prefsdlg.h"
#include "tagtypes.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QVBoxLayout>

PrefsDialog::PrefsDialog(const AppSettings &current, QWidget *parent)
    : QDialog(parent), m_result(current), m_accent(current.accent) {
    setWindowTitle(QStringLiteral("Preferences"));
    setModal(true);
    resize(700, 780);
    auto *outer = new QVBoxLayout(this);
    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto *page = new QWidget(this);
    auto *lay = new QVBoxLayout(page);
    lay->setContentsMargins(0, 0, 8, 0);
    scroll->setWidget(page);
    outer->addWidget(scroll, 1);

    // ── General ──
    auto *gen = new QGroupBox(QStringLiteral("General"), this);
    auto *genLay = new QVBoxLayout(gen);
    m_restore = new QCheckBox(
        QStringLiteral("Always load last session on startup"), this);
    m_restore->setChecked(current.restoreSession);
    auto *histRow = new QHBoxLayout();
    histRow->addWidget(new QLabel(QStringLiteral("Archive history:"), this));
    histRow->addStretch(1);
    auto *histClear = new QPushButton(QStringLiteral("Clear history"), this);
    connect(histClear, &QPushButton::clicked, this, [this] {
        m_result.history.clear();
        QMessageBox::information(nullptr, QStringLiteral("History"),
                                 QStringLiteral("Archive history cleared."));
    });
    histRow->addWidget(histClear);
    genLay->addWidget(m_restore);
    genLay->addLayout(histRow);
    lay->addWidget(gen);

    // ── Library / cache ──
    auto *lib = new QGroupBox(QStringLiteral("Library cache"), this);
    auto *libLay = new QVBoxLayout(lib);
    m_indexCache = new QCheckBox(
        QStringLiteral("Scan index cache (fast rescans of large archives)"), this);
    m_thumbCache = new QCheckBox(QStringLiteral("Thumbnail disk cache"), this);
    m_indexCache->setChecked(current.indexCache);
    m_thumbCache->setChecked(current.thumbCache);
    m_classify = new QCheckBox(
        QStringLiteral("Classify untyped tags online (Safebooru/yandere, cached)"),
        this);
    m_classify->setChecked(current.classifyTags);
    auto *sizeRow = new QHBoxLayout();
    sizeRow->addWidget(new QLabel(QStringLiteral("Thumbnail size:"), this));
    m_thumbSize = new QComboBox(this);
    for (int s : {256, 360, 512, 768})
        m_thumbSize->addItem(QStringLiteral("%1 px").arg(s), s);
    m_thumbSize->setCurrentIndex(
        qMax(0, m_thumbSize->findData(current.thumbLongSide)));
    sizeRow->addWidget(m_thumbSize);
    sizeRow->addStretch(1);
    auto *clearBtn = new QPushButton(QStringLiteral("Clear caches…"), this);
    connect(clearBtn, &QPushButton::clicked, this, [] {
        clearCaches();
        QMessageBox::information(
            nullptr, QStringLiteral("Caches"),
            QStringLiteral("Scan index and thumbnail caches cleared.\n"
                           "They rebuild on the next scan."));
    });
    sizeRow->addWidget(clearBtn);
    libLay->addWidget(m_indexCache);
    libLay->addWidget(m_thumbCache);
    auto note = [this](const QString &text) {
        auto *l = new QLabel(text, this);
        l->setWordWrap(true);
        return l;
    };
    libLay->addWidget(m_classify);
    libLay->addWidget(note(QStringLiteral(
        "Untyped tags (tagType \"none\", Gelbooru blobs) get their category "
        "from public tag APIs. Results cache forever; misses stay general.")));
    libLay->addLayout(sizeRow);
    libLay->addWidget(note(QStringLiteral(
        "Cache toggles take effect on the next scan.")));
    lay->addWidget(lib);

    // ── Gelbooru API (optional) ──
    auto *gel = new QGroupBox(QStringLiteral("Gelbooru API (optional)"), this);
    auto *gelLay = new QFormLayout(gel);
    m_gelUser = new QLineEdit(current.gelbooruUser, this);
    m_gelUser->setPlaceholderText(QStringLiteral("numeric user ID"));
    m_gelKey = new QLineEdit(current.gelbooruKey, this);
    m_gelKey->setPlaceholderText(QStringLiteral("API key"));
    m_gelKey->setEchoMode(QLineEdit::Password);
    gelLay->addRow(QStringLiteral("User ID:"), m_gelUser);
    gelLay->addRow(QStringLiteral("API key:"), m_gelKey);
    auto *gelRow = new QHBoxLayout();
    auto *gelTest = new QPushButton(QStringLiteral("Test credentials"), this);
    m_gelStatus = new QLabel(this);
    m_gelStatus->setStyleSheet(QStringLiteral("color:#4a5068;"));
    connect(gelTest, &QPushButton::clicked, this, [this] {
        m_gelStatus->setText(QStringLiteral("Testing…"));
        // One-shot checker; deleted with the dialog either way.
        auto *probe = new TagTypeFetcher(this);
        connect(probe, &TagTypeFetcher::validated, this,
                [this, probe](bool ok, const QString &msg) {
                    m_gelStatus->setText((ok ? QStringLiteral("✓ ")
                                            : QStringLiteral("✗ ")) +
                                         msg);
                    probe->deleteLater();
                });
        probe->validateCredentials(m_gelUser->text(), m_gelKey->text());
    });
    gelRow->addWidget(gelTest);
    gelRow->addWidget(m_gelStatus, 1);
    gelLay->addRow(QStringLiteral(""), gelRow);
    gelLay->addRow(QStringLiteral(""), note(QStringLiteral(
        "Both fields are required. When set, Gelbooru is queried first for "
        "tag categories; the public sources stay as fallback. Stored "
        "plaintext in settings.ini.")));
    lay->addWidget(gel);

    // ── Appearance ──
    auto *app = new QGroupBox(QStringLiteral("Appearance"), this);
    auto *appLay = new QFormLayout(app);
    m_themeMode = new QComboBox(this);
    m_themeMode->addItem(QStringLiteral("Built-in dark (Fusion, recommended)"),
                         QStringLiteral("builtin"));
    m_themeMode->addItem(QStringLiteral("System native"), QStringLiteral("native"));
    m_themeMode->setCurrentIndex(current.themeMode == QStringLiteral("native") ? 1 : 0);
    appLay->addRow(QStringLiteral("Theme:"), m_themeMode);
    m_accentBtn = new QPushButton(QStringLiteral("Pick…"), this);
    setSwatch(m_accent);
    connect(m_accentBtn, &QPushButton::clicked, this, [this] {
        const QColor c =
            QColorDialog::getColor(m_accent, this, QStringLiteral("Accent color"));
        if (c.isValid()) {
            m_accent = c;
            setSwatch(c);
        }
    });
    auto *accRow = new QHBoxLayout();
    accRow->addWidget(m_accentBtn);
    accRow->addWidget(new QLabel(
        QStringLiteral("Highlights, selections, progress bars"), this));
    accRow->addStretch(1);
    appLay->addRow(QStringLiteral("Accent:"), accRow);
    m_stylesheet = new QLineEdit(current.stylesheet, this);
    m_stylesheet->setPlaceholderText(QStringLiteral("Optional custom .qss"));
    auto *ssBrowse = new QPushButton(QStringLiteral("Browse…"), this);
    connect(ssBrowse, &QPushButton::clicked, this, [this] {
        const QString p = QFileDialog::getOpenFileName(
            this, QStringLiteral("Custom stylesheet"), m_stylesheet->text(),
            QStringLiteral("Stylesheets (*.qss);;All files (*)"));
        if (!p.isEmpty())
            m_stylesheet->setText(p);
    });
    auto *ssClear = new QPushButton(QStringLiteral("Clear"), this);
    connect(ssClear, &QPushButton::clicked, m_stylesheet, &QLineEdit::clear);
    auto *ssRow = new QHBoxLayout();
    ssRow->addWidget(m_stylesheet, 1);
    ssRow->addWidget(ssBrowse);
    ssRow->addWidget(ssClear);
    appLay->addRow(QStringLiteral("Custom .qss:"), ssRow);
    lay->addWidget(app);

    // ── Grid ──
    auto *grid = new QGroupBox(QStringLiteral("Grid"), this);
    auto *gridLay = new QFormLayout(grid);
    m_aspect = new QComboBox(this);
    const QList<QPair<QString, QVariantList>> ratios = {
        {QStringLiteral("Square 1:1"), {1, 1}},
        {QStringLiteral("Standard 4:3"), {4, 3}},
        {QStringLiteral("Photo 3:2"), {3, 2}},
        {QStringLiteral("Wide 16:9"), {16, 9}},
        {QStringLiteral("Portrait 3:4"), {3, 4}},
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
            QStringLiteral("Custom %1:%2").arg(m_customNum).arg(m_customDen),
            QVariantList{m_customNum, m_customDen});
        aspectIdx = m_aspect->count() - 1;
    }
    m_aspect->setCurrentIndex(aspectIdx);
    gridLay->addRow(QStringLiteral("Thumbnail ratio:"), m_aspect);
    lay->addWidget(grid);

    // ── Explicit detection ──
    auto *exp = new QGroupBox(QStringLiteral("Explicit detection"), this);
    auto *expLay = new QVBoxLayout(exp);
    expLay->addWidget(note(QStringLiteral(
        "Non-explicit posts carrying any of these tags are shown as explicit "
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
    auto *expDefaults = new QPushButton(QStringLiteral("Restore defaults"), this);
    connect(expDefaults, &QPushButton::clicked, this, [this] {
        m_explicit->setPlainText(defaultExplicitTagList().join(u'\n'));
    });
    expRow->addWidget(m_explicitCount, 1);
    expRow->addWidget(expDefaults);
    expLay->addLayout(expRow);
    auto updateExpCount = [this] {
        const int n = normalizeExplicitTags(
                          m_explicit->toPlainText().split(u'\n')).size();
        m_explicitCount->setText(QStringLiteral("%1 tags (one per line)").arg(n));
    };
    connect(m_explicit, &QPlainTextEdit::textChanged, this, updateExpCount);
    updateExpCount();
    lay->addWidget(exp, 1);

    // ── Playback ──
    auto *play = new QGroupBox(QStringLiteral("Video playback"), this);
    auto *playLay = new QVBoxLayout(play);
    m_muted = new QCheckBox(QStringLiteral("Mute videos by default"), this);
    m_loop = new QCheckBox(QStringLiteral("Loop videos"), this);
    m_muted->setChecked(current.videoMuted);
    m_loop->setChecked(current.videoLoop);
    auto *volRow = new QHBoxLayout();
    volRow->addWidget(new QLabel(QStringLiteral("Default volume:"), this));
    m_volume = new QSlider(Qt::Horizontal, this);
    m_volume->setRange(0, 100);
    m_volume->setValue(current.volume);
    volRow->addWidget(m_volume, 1);
    playLay->addWidget(m_muted);
    playLay->addWidget(m_loop);
    playLay->addLayout(volRow);
    lay->addWidget(play);

    // ── Viewer ──
    auto *view = new QGroupBox(QStringLiteral("Viewer"), this);
    auto *viewLay = new QFormLayout(view);
    m_maxSide = new QComboBox(this);
    for (int s : {1280, 1920, 2560, 3840})
        m_maxSide->addItem(QStringLiteral("%1 px longest side").arg(s), s);
    m_maxSide->setCurrentIndex(qMax(0, m_maxSide->findData(current.viewerMaxSide)));
    viewLay->addRow(QStringLiteral("Huge-image downscale:"), m_maxSide);
    lay->addWidget(view);

    auto *buttons = new QDialogButtonBox(this);
    buttons->setStandardButtons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel |
                                QDialogButtonBox::RestoreDefaults);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        harvest();
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons->button(QDialogButtonBox::RestoreDefaults), &QPushButton::clicked,
            this, [this] {
                const AppSettings d;
                m_restore->setChecked(d.restoreSession);
                m_classify->setChecked(d.classifyTags);
                m_gelUser->clear();
                m_gelKey->clear();
                m_indexCache->setChecked(d.indexCache);
                m_thumbCache->setChecked(d.thumbCache);
                m_thumbSize->setCurrentIndex(qMax(0, m_thumbSize->findData(d.thumbLongSide)));
                m_themeMode->setCurrentIndex(0);
                m_accent = QColor(d.accent);
                setSwatch(m_accent);
                m_stylesheet->clear();
                m_aspect->setCurrentIndex(0);
                m_muted->setChecked(d.videoMuted);
                m_loop->setChecked(d.videoLoop);
                m_volume->setValue(d.volume);
                m_maxSide->setCurrentIndex(
                    qMax(0, m_maxSide->findData(d.viewerMaxSide)));
                m_explicit->setPlainText(d.explicitTags.join(u'\n'));
            });
    // Buttons live outside the scroll area so they stay visible.
    outer->addWidget(buttons);
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

void PrefsDialog::harvest() {
    m_result.restoreSession = m_restore->isChecked();
    m_result.classifyTags = m_classify->isChecked();
    m_result.gelbooruUser = m_gelUser->text().trimmed();
    m_result.gelbooruKey = m_gelKey->text().trimmed();
    m_result.indexCache = m_indexCache->isChecked();
    m_result.thumbCache = m_thumbCache->isChecked();
    m_result.thumbLongSide = m_thumbSize->currentData().toInt();
    m_result.themeMode = m_themeMode->currentData().toString();
    m_result.accent = m_accent.name();
    m_result.stylesheet = m_stylesheet->text().trimmed();
    const QVariantList r = m_aspect->currentData().toList();
    if (r.size() == 2) {
        m_result.aspectNum = qMax(1, r.value(0).toInt());
        m_result.aspectDen = qMax(1, r.value(1).toInt());
    }
    m_result.videoMuted = m_muted->isChecked();
    m_result.videoLoop = m_loop->isChecked();
    m_result.volume = m_volume->value();
    m_result.viewerMaxSide = m_maxSide->currentData().toInt();
    m_result.explicitTags =
        normalizeExplicitTags(m_explicit->toPlainText().split(u'\n'));
}
