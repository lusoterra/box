#include "welcomepage.h"

#include "appinfo.h"
#include "uicons.h"

#include <QCheckBox>
#include <QDir>
#include <QFileInfo>
#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPixmap>
#include <QPushButton>
#include <QResizeEvent>
#include <QVBoxLayout>

WelcomePage::WelcomePage(QWidget *parent) : QWidget(parent) {
    auto *outer = new QVBoxLayout(this);
    outer->addStretch(1);
    auto *center = new QHBoxLayout();
    center->addStretch(1);
    auto *col = new QVBoxLayout();
    col->setSpacing(10);
    center->addLayout(col, 2); // narrow column, stretches take the rest
    center->addStretch(1);
    outer->addLayout(center);
    outer->addStretch(2);

    auto *titleRow = new QHBoxLayout();
    auto *icon = new QLabel(this);
    QPixmap pm(QStringLiteral(":/icons/app.png"));
    if (!pm.isNull())
        icon->setPixmap(pm.scaled(64, 64, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    auto *title = new QLabel(tr("booru-explorer"), this);
    QFont tf = title->font();
    tf.setPointSize(tf.pointSize() + 12);
    tf.setBold(true);
    title->setFont(tf);
    titleRow->addStretch(1);
    titleRow->addWidget(icon);
    titleRow->addWidget(title);
    titleRow->addStretch(1);
    col->addLayout(titleRow);
    auto *subtitle =
        new QLabel(appTagline() + QStringLiteral(" · v") + appVersion(), this);
    subtitle->setAlignment(Qt::AlignCenter);
    subtitle->setStyleSheet(QStringLiteral("color:#8a90a8;"));
    col->addWidget(subtitle);

    m_resume = new QPushButton(tr("Back to last tab"), this);
    m_resume->setIcon(UiIcons::icon(QStringLiteral("arrow-left")));
    m_resume->setIconSize(QSize(16, 16));
    m_resume->setMinimumHeight(44);
    m_resume->setVisible(false);
    connect(m_resume, &QPushButton::clicked, this, &WelcomePage::resumeRequested);
    col->addWidget(m_resume);
    auto *browseBtn = new QPushButton(tr("New browse tab"), this);
    browseBtn->setIcon(UiIcons::icon(QStringLiteral("plus")));
    browseBtn->setIconSize(QSize(16, 16));
    browseBtn->setMinimumHeight(44);
    browseBtn->setToolTip(tr("Search Danbooru, Gelbooru and more online"));
    connect(browseBtn, &QPushButton::clicked, this, &WelcomePage::browseTabRequested);
    col->addWidget(browseBtn);
    auto *favBtn = new QPushButton(tr("Favourites"), this);
    favBtn->setIcon(UiIcons::heart(UiIcons::favRed(), true));
    favBtn->setIconSize(QSize(16, 16));
    favBtn->setMinimumHeight(44);
    connect(favBtn, &QPushButton::clicked, this, &WelcomePage::favTabRequested);
    col->addWidget(favBtn);
    auto *dlBtn = new QPushButton(tr("Downloads"), this);
    dlBtn->setIcon(UiIcons::icon(QStringLiteral("download")));
    dlBtn->setIconSize(QSize(16, 16));
    dlBtn->setMinimumHeight(44);
    dlBtn->setToolTip(tr("Browse the download folder"));
    connect(dlBtn, &QPushButton::clicked, this, &WelcomePage::downloadsRequested);
    col->addWidget(dlBtn);

    m_always = new QCheckBox(tr("Always load last session on startup"), this);
    connect(m_always, &QCheckBox::toggled, this, &WelcomePage::restoreToggled);
    col->addWidget(m_always);

    // Background mascot (home page only): clicks pass through, content
    // stays above it.
    m_mascot = new QLabel(this);
    m_mascot->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_mascot->setScaledContents(false);
    m_mascot->setAlignment(Qt::AlignBottom | Qt::AlignRight);
    auto *dim = new QGraphicsOpacityEffect(m_mascot);
    dim->setOpacity(0.9);
    m_mascot->setGraphicsEffect(dim);
    m_mascot->lower();
    m_mascot->hide();
}

void WelcomePage::setMascot(const QString &path) {
    m_mascotFull = QPixmap(path);
    if (m_mascotFull.isNull())
        m_mascot->hide();
    layoutMascot();
}

void WelcomePage::resizeEvent(QResizeEvent *e) {
    QWidget::resizeEvent(e);
    layoutMascot();
}

void WelcomePage::layoutMascot() {
    if (m_mascotFull.isNull()) {
        m_mascot->hide();
        return;
    }
    // Bottom-right, never taller than ~45% of the page.
    const int h = qMax(64, qMin(360, height() * 45 / 100));
    const QPixmap scaled = m_mascotFull.scaledToHeight(
        h, Qt::SmoothTransformation);
    m_mascot->setPixmap(scaled);
    m_mascot->setGeometry(width() - scaled.width() - 24,
                          height() - scaled.height() - 16,
                          scaled.width(), scaled.height());
    m_mascot->show();
}

void WelcomePage::setRestore(bool on) {
    // Checked when the default tab is the last session.
    m_always->blockSignals(true);
    m_always->setChecked(on);
    m_always->blockSignals(false);
}

void WelcomePage::setLastPlace(const QString &label) {
    if (!m_resume)
        return;
    m_resume->setVisible(!label.isEmpty());
    if (!label.isEmpty())
        m_resume->setToolTip(label);
}
