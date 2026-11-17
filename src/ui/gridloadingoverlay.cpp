#include "gridloadingoverlay.h"

#include "progressbar.h"

#include <QEvent>
#include <QLabel>
#include <QPainter>
#include <QVBoxLayout>

GridLoadingOverlay::GridLoadingOverlay(QWidget *parent) : QWidget(parent) {
    setObjectName(QStringLiteral("gridLoadingOverlay"));
    // Opaque enough to hide stale cards from the previous tab.
    setAttribute(Qt::WA_OpaquePaintEvent, false);
    setAttribute(Qt::WA_NoSystemBackground, true);
    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(24, 24, 24, 24);
    lay->addStretch(1);
    m_bar = new CenterProgressBar(this);
    m_bar->setRange(0, 0); // indeterminate: Qt animates the chunk
    m_bar->setTextVisible(false);
    m_bar->setFixedWidth(220);
    m_bar->setMaximumHeight(12);
    lay->addWidget(m_bar, 0, Qt::AlignHCenter);
    m_label = new QLabel(this);
    m_label->setAlignment(Qt::AlignCenter);
    lay->addSpacing(10);
    lay->addWidget(m_label, 0, Qt::AlignHCenter);
    lay->addStretch(1);
    hide();
    if (parent)
        parent->installEventFilter(this);
}

bool GridLoadingOverlay::eventFilter(QObject *watched, QEvent *e) {
    if (watched == parentWidget() && e->type() == QEvent::Resize &&
        isVisible())
        setGeometry(parentWidget()->rect());
    return QWidget::eventFilter(watched, e);
}

void GridLoadingOverlay::showFor(const QString &what) {
    m_label->setText(what);
    if (m_bar->minimum() != 0 || m_bar->maximum() != 0)
        m_bar->setRange(0, 0);
    if (parentWidget())
        setGeometry(parentWidget()->rect());
    raise();
    show();
    // Paint before the caller returns: the tab's heavy work is queued on
    // the next event-loop turn and would otherwise hit a blank window.
    repaint();
    m_bar->repaint();
    m_label->repaint();
}

void GridLoadingOverlay::paintEvent(QPaintEvent *) {
    QPainter p(this);
    QColor bg = palette().color(QPalette::Base);
    bg.setAlpha(235);
    p.fillRect(rect(), bg);
}
