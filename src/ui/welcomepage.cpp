#include "welcomepage.h"

#include "appinfo.h"
#include "uicons.h"

#include <QApplication>
#include <QCheckBox>
#include <QDir>
#include <QFileInfo>
#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMainWindow>
#include <QMouseEvent>
#include <QPixmap>
#include <QShowEvent>
#include <QStatusBar>
#include <QPushButton>
#include <QResizeEvent>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>

namespace {
// Long enough to feel like the app is sleeping, short enough to catch
// someone leaving the home tab open.
constexpr int kMascotIdleMs = 180 * 1000;
constexpr int kMascotBounceMs = 16;
// Roughly 130 x 110 px/s at the 60 fps tick above: a lazy DVD drift.
constexpr qreal kMascotSpeedX = 2.2;
constexpr qreal kMascotSpeedY = 1.8;

bool isActivityEvent(QEvent::Type type) {
    switch (type) {
    case QEvent::MouseButtonPress:
    case QEvent::KeyPress:
    case QEvent::Wheel:
    case QEvent::TouchBegin:
    case QEvent::TabletPress:
        return true;
    default:
        return false;
    }
}
} // namespace

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
    QPixmap pm(QStringLiteral(":/icons/app.svg"));
    if (!pm.isNull())
        icon->setPixmap(pm.scaled(64, 64, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    auto *title = new QLabel(tr("Box"), this);
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

    m_resume = new QPushButton(tr("Back to last session"), this);
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
    // stays above it. Double-click cycles the size (persisted via
    // mascotScaleChanged).
    m_mascot = new QLabel(this);
    m_mascot->setAttribute(Qt::WA_TransparentForMouseEvents, false);
    m_mascot->installEventFilter(this);
    m_mascot->setScaledContents(false);
    m_mascot->setAlignment(Qt::AlignBottom | Qt::AlignRight);
    m_mascotDim = new QGraphicsOpacityEffect(m_mascot);
    m_mascotDim->setOpacity(0.9);
    m_mascot->setGraphicsEffect(m_mascotDim);
    m_mascot->lower();
    m_mascot->hide();

    m_mascotIdle = new QTimer(this);
    m_mascotIdle->setSingleShot(true);
    m_mascotIdle->setInterval(kMascotIdleMs);
    connect(m_mascotIdle, &QTimer::timeout, this, &WelcomePage::startMascotBounce);
    m_mascotBounce = new QTimer(this);
    m_mascotBounce->setInterval(kMascotBounceMs);
    connect(m_mascotBounce, &QTimer::timeout, this,
            &WelcomePage::stepMascotBounce);
}

void WelcomePage::setMascot(const QString &path) {
    m_mascotFull = QPixmap(path);
    if (m_mascotFull.isNull()) {
        m_mascotIdle->stop();
        stopMascotBounce();
        m_mascot->hide();
    } else if (isVisible()) {
        m_mascotIdle->start();
    }
    layoutMascot();
}

void WelcomePage::setMascotOpacity(double opacity) {
    if (m_mascotDim)
        m_mascotDim->setOpacity(qBound(0.1, opacity, 1.0));
}

void WelcomePage::setMascotScale(double scale) {
    m_mascotScale = qBound(0.1, scale, 2.0);
    stopMascotBounce();
    layoutMascot();
}

void WelcomePage::setMascotMargin(int px) {
    m_mascotMargin = qBound(0, px, 256);
    stopMascotBounce();
    layoutMascot();
}

bool WelcomePage::eventFilter(QObject *watched, QEvent *e) {
    // Shift+left click is the easter-egg / testing trigger: start the
    // bounce right away instead of treating the click as plain activity.
    if (watched == m_mascot && e->type() == QEvent::MouseButtonPress) {
        auto *me = static_cast<QMouseEvent *>(e);
        if (me->button() == Qt::LeftButton &&
            me->modifiers().testFlag(Qt::ShiftModifier)) {
            startMascotBounce();
            return true;
        }
    }
    // Double-click the mascot cycles small -> medium -> large ->
    // max height -> full width.
    if (watched == m_mascot && e->type() == QEvent::MouseButtonDblClick) {
        static constexpr double steps[] = {0.25, 0.45, 0.65, 1.0, 2.0};
        // First step above the current size; wraps to small past large.
        double next = steps[0];
        for (double s : steps) {
            if (s > m_mascotScale + 1e-9) {
                next = s;
                break;
            }
        }
        m_mascotScale = next;
        layoutMascot();
        emit mascotScaleChanged(next);
        return true;
    }
    if (isActivityEvent(e->type()))
        noteActivity();
    return QWidget::eventFilter(watched, e);
}

void WelcomePage::resizeEvent(QResizeEvent *e) {
    QWidget::resizeEvent(e);
    noteActivity();
    layoutMascot();
}

void WelcomePage::showEvent(QShowEvent *e) {
    QWidget::showEvent(e);
    // App-wide filter: clicks/keys/wheel anywhere on the home screen count
    // as activity. Window moves are not in this set on purpose.
    if (qApp)
        qApp->installEventFilter(this);
    if (!m_mascotFull.isNull())
        m_mascotIdle->start();
    layoutMascot();
}

void WelcomePage::hideEvent(QHideEvent *e) {
    QWidget::hideEvent(e);
    if (qApp)
        qApp->removeEventFilter(this);
    m_mascotIdle->stop();
    stopMascotBounce();
}

void WelcomePage::noteActivity() {
    if (!isVisible())
        return;
    if (m_mascotBouncing)
        stopMascotBounce();
    if (!m_mascotFull.isNull())
        m_mascotIdle->start();
}

void WelcomePage::startMascotBounce() {
    if (m_mascotFull.isNull() || !isVisible() || m_mascotBouncing)
        return;
    layoutMascot();
    if (!m_mascot->isVisible())
        return;
    m_mascotIdle->stop();
    m_mascotBouncing = true;
    m_mascotPos = m_mascot->pos();
    // Head up-left so leaving the corner reads as a DVD-logo hop.
    m_mascotVel = QPointF(-kMascotSpeedX, -kMascotSpeedY);
    m_mascotBounce->start();
}

void WelcomePage::stopMascotBounce() {
    if (!m_mascotBouncing)
        return;
    m_mascotBouncing = false;
    m_mascotBounce->stop();
    layoutMascot();
}

void WelcomePage::stepMascotBounce() {
    if (!m_mascotBouncing)
        return;
    const QSize sz = m_mascot->size();
    const qreal maxX = qMax(0, width() - sz.width());
    const qreal maxY = qMax(0, height() - sz.height());
    QPointF next = m_mascotPos + m_mascotVel;
    if (next.x() <= 0) {
        next.setX(0);
        m_mascotVel.setX(qAbs(m_mascotVel.x()));
    } else if (next.x() >= maxX) {
        next.setX(maxX);
        m_mascotVel.setX(-qAbs(m_mascotVel.x()));
    }
    if (next.y() <= 0) {
        next.setY(0);
        m_mascotVel.setY(qAbs(m_mascotVel.y()));
    } else if (next.y() >= maxY) {
        next.setY(maxY);
        m_mascotVel.setY(-qAbs(m_mascotVel.y()));
    }
    m_mascotPos = next;
    m_mascot->move(next.toPoint());
}

void WelcomePage::layoutMascot() {
    if (m_mascotFull.isNull() || width() <= 0 || height() <= 0) {
        if (m_mascotFull.isNull())
            m_mascot->hide();
        return;
    }
    // The page ends where the main-window status bar begins: measure that
    // strip live so the image pins to the WINDOW's bottom-right (tucked
    // behind the status text), not the page's. The margin insets the
    // anchor corner; the image box's bottom-right always lands on
    // (right, bottom) below. No stretching anywhere: aspect is preserved,
    // overflow is cropped top/left.
    int strip = 0;
    if (QMainWindow *mw = qobject_cast<QMainWindow *>(window())) {
        if (QWidget *sb = mw->statusBar(); sb && sb->isVisible())
            strip = sb->height();
    }
    const int right = width() - m_mascotMargin;
    const int bottom = height() + strip - m_mascotMargin;
    if (m_mascotScale >= 1.99) {
        // Full width: window wide, overflow cropped top.
        const QPixmap scaled = m_mascotFull.scaledToWidth(
            qMax(1, right), Qt::SmoothTransformation);
        m_mascot->setPixmap(scaled);
        m_mascot->setGeometry(0, bottom - scaled.height(), right,
                              scaled.height());
    } else {
        // Height fraction, or max height at 1.0.
        const int h = m_mascotScale >= 0.99
                          ? height() + strip
                          : qMax(64, int(height() * m_mascotScale));
        const QPixmap scaled = m_mascotFull.scaledToHeight(
            h, Qt::SmoothTransformation);
        m_mascot->setPixmap(scaled);
        m_mascot->setGeometry(right - scaled.width(), bottom - scaled.height(),
                              scaled.width(), scaled.height());
    }
    m_mascot->show();
    m_mascot->lower(); // always behind the buttons
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
