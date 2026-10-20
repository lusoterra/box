#include "tabbar.h"

#include <QApplication>
#include <QStyleOptionTab>
#include <QStylePainter>

MarqueeTabBar::MarqueeTabBar(QWidget *parent) : QTabBar(parent) {
    setElideMode(Qt::ElideRight);
    m_timer = new QTimer(this);
    m_timer->setInterval(120);
    connect(m_timer, &QTimer::timeout, this, &MarqueeTabBar::onTick);
    connect(this, &QTabBar::currentChanged, this, [this] {
        m_offset = 0;
        m_timer->start();
    });
    connect(this, &QTabBar::tabMoved, this, &MarqueeTabBar::onTabMoved);
}

void MarqueeTabBar::setFullTitle(int index, const QString &title) {
    if (index < 0)
        return;
    while (m_fullTitles.size() <= index)
        m_fullTitles.append(QString());
    m_fullTitles[index] = title;
    setTabText(index, title); // eliding still sizes the tab sanely
    setTabToolTip(index, title);
    m_timer->start();
}

QString MarqueeTabBar::fullTitle(int index) const {
    if (index < 0 || index >= m_fullTitles.size() ||
        m_fullTitles.at(index).isEmpty())
        return tabText(index);
    return m_fullTitles.at(index);
}

void MarqueeTabBar::tabInserted(int index) {
    m_fullTitles.insert(qBound(0, index, m_fullTitles.size()), QString());
}

void MarqueeTabBar::tabRemoved(int index) {
    if (index >= 0 && index < m_fullTitles.size())
        m_fullTitles.removeAt(index);
    m_offset = 0;
}

void MarqueeTabBar::onTabMoved(int from, int to) {
    m_dragging = false;
    if (from < 0 || to < 0 || from >= m_fullTitles.size() ||
        to >= m_fullTitles.size() || from == to)
        return;
    m_fullTitles.move(from, to);
    m_offset = 0;
}

void MarqueeTabBar::mousePressEvent(QMouseEvent *e) {
    if (e->button() == Qt::LeftButton && isMovable()) {
        m_pressed = true;
        m_pressPos = e->pos();
    }
    QTabBar::mousePressEvent(e);
}

void MarqueeTabBar::mouseMoveEvent(QMouseEvent *e) {
    if (m_pressed && isMovable() &&
        (e->pos() - m_pressPos).manhattanLength() >=
            QApplication::startDragDistance()) {
        m_dragging = true;
        update();
    }
    QTabBar::mouseMoveEvent(e);
}

void MarqueeTabBar::mouseReleaseEvent(QMouseEvent *e) {
    m_pressed = false;
    if (m_dragging) {
        m_dragging = false;
        update();
    }
    QTabBar::mouseReleaseEvent(e);
}

namespace {
// Native (unshrunk) icon width at the tab bar's icon height, in device
// pixels divided out: multi-mark combos are wider than one slot, and the
// paint below draws them full-size instead of crushing them into 16px.
int nativeIconWidth(const QTabBar *bar, const QIcon &icon) {
    if (icon.isNull())
        return 0;
    const int h = qMax(1, bar->iconSize().height());
    const QPixmap pm = icon.pixmap(QSize(4096, h));
    const qreal dpr = pm.devicePixelRatio();
    return qMax(1, int(pm.width() / (dpr > 0 ? dpr : 1.0)));
}
} // namespace

QSize MarqueeTabBar::tabSizeHint(int index) const {
    QSize s = QTabBar::tabSizeHint(index);
    const int extra =
        nativeIconWidth(this, tabIcon(index)) - qMax(1, iconSize().width());
    if (extra > 0)
        s.setWidth(s.width() + extra);
    return s;
}

bool MarqueeTabBar::tabOverflows(int index) const {
    const QString full = fullTitle(index);
    if (full.isEmpty())
        return false;
    QStyleOptionTab opt;
    initStyleOption(&opt, index);
    QRect textRect = style()->subElementRect(QStyle::SE_TabBarTabText, &opt, this);
    // (textRect already excludes the icon area - no adjustment needed.)
    return fontMetrics().horizontalAdvance(full) > textRect.width();
}

void MarqueeTabBar::onTick() {
    // Animate while any tab overflows; else stay parked.
    for (int i = 0; i < count(); ++i) {
        if (tabOverflows(i)) {
            m_offset += 2;
            update();
            return;
        }
    }
    if (m_offset != 0) {
        m_offset = 0;
        update();
    }
}

void MarqueeTabBar::paintEvent(QPaintEvent *e) {
    if (m_dragging) {
        // Reorder in flight: the native style owns the dragged-tab
        // pixmap and the drop indicator, so let it paint.
        QTabBar::paintEvent(e);
        return;
    }
    QStylePainter p(this);
    for (int i = 0; i < count(); ++i) {
        QStyleOptionTab opt;
        initStyleOption(&opt, i);
        p.drawControl(QStyle::CE_TabBarTabShape, opt);
        const bool selected = opt.state & QStyle::State_Selected;
        if (selected) {
            // Accent bar along the bottom edge: the native selected-shape
            // wash is too subtle at a glance, this reads instantly.
            const QColor acc = opt.palette.color(QPalette::Highlight);
            const QRect r = opt.rect;
            p.fillRect(QRect(r.left() + 4, r.bottom() - 3,
                             r.width() - 8, 3),
                       acc);
        }
        const QString full = fullTitle(i);
        QRect textRect = style()->subElementRect(QStyle::SE_TabBarTabText, &opt, this);
        // Tab icon (CE_TabBarTabLabel is skipped, so draw it ourselves).
        // Drawn at native width: combos wider than one slot keep every
        // mark full-size, and the text shifts right into the extra width
        // tabSizeHint() reserved (single icons are unaffected).
        QRect textR = textRect;
        if (!opt.icon.isNull()) {
            const int iconW = nativeIconWidth(this, opt.icon);
            const int shift = qMax(0, iconW - qMax(1, opt.iconSize.width()));
            const QPixmap pm = opt.icon.pixmap(
                QSize(qMax(1, iconW), qMax(1, opt.iconSize.height())));
            const qreal dpr = pm.devicePixelRatio();
            p.drawPixmap(qRound(textRect.left() + shift - pm.width() / dpr - 5),
                         qRound(textRect.center().y() - pm.height() / dpr / 2), pm);
            textR.translate(shift, 0);
        }
        p.save();
        p.setClipRect(textR);
        p.setPen(opt.palette.color(
            (opt.state & QStyle::State_Selected) ? QPalette::HighlightedText
                                                 : QPalette::WindowText));
        const QFontMetrics fm = opt.fontMetrics;
        if (selected) {
            QFont f = p.font();
            f.setBold(true);
            p.setFont(f);
        }
        const int fw = fm.horizontalAdvance(full);
        if (fw > textR.width()) {
            // Scroll with wraparound ("title    title") and a dwell pause
            // at the start of each loop.
            const QString cycle = full + QStringLiteral("    ");
            const int cw = qMax(1, fm.horizontalAdvance(cycle));
            const int dwell = 12; // ticks parked at the start
            const int phase = m_offset % (cw + dwell);
            const int x = (phase < dwell) ? textR.left()
                                          : textR.left() - (phase - dwell);
            p.drawText(QRect(x, textR.top(), cw + textR.width() + cw,
                             textR.height()),
                       Qt::AlignLeft | Qt::AlignVCenter, cycle + full);
        } else {
            p.drawText(textR, Qt::AlignLeft | Qt::AlignVCenter,
                       fm.elidedText(full, Qt::ElideRight,
                                                  textR.width()));
        }
        p.restore();
    }
}
