#include "tabbar.h"

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
}

void MarqueeTabBar::setFullTitle(int index, const QString &title) {
    m_titles[index] = title;
    setTabText(index, title); // eliding still sizes the tab sanely
    setTabToolTip(index, title);
    m_timer->start();
}

QString MarqueeTabBar::fullTitle(int index) const {
    return m_titles.value(index, tabText(index));
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

void MarqueeTabBar::paintEvent(QPaintEvent *) {
    QStylePainter p(this);
    for (int i = 0; i < count(); ++i) {
        QStyleOptionTab opt;
        initStyleOption(&opt, i);
        p.drawControl(QStyle::CE_TabBarTabShape, opt);
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
