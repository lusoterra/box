#include "progressbar.h"

#include <QPaintEvent>
#include <QStyleOptionProgressBar>
#include <QStylePainter>

void CenterProgressBar::paintEvent(QPaintEvent *) {
    QStylePainter p(this);
    QStyleOptionProgressBar opt;
    initStyleOption(&opt);
    // Frame + fill first; the label is drawn manually below.
    opt.textVisible = false;
    p.drawControl(QStyle::CE_ProgressBarGroove, opt);
    p.drawControl(QStyle::CE_ProgressBarContents, opt);
    if (!isTextVisible())
        return;
    const QString label = text();
    if (label.isEmpty())
        return;
    const QRect groove =
        style()->subElementRect(QStyle::SE_ProgressBarGroove, &opt, this);
    const QRect contents =
        style()->subElementRect(QStyle::SE_ProgressBarContents, &opt, this);
    const int lo = minimum(), hi = maximum();
    const double frac =
        (hi > lo) ? double(value() - lo) / double(hi - lo) : 0.0;
    const int tw = p.fontMetrics().horizontalAdvance(label);
    const int th = p.fontMetrics().height();
    constexpr int gap = 6;
    p.save();
    p.setClipRect(groove);
    if (orientation() == Qt::Vertical) {
        p.setPen(palette().color(QPalette::WindowText));
        p.drawText(groove, Qt::AlignCenter, label);
    } else {
        const QRect fill(contents.left(), contents.top(),
                         int(contents.width() * frac), contents.height());
        if (fill.width() >= tw + gap * 2) {
            // Room: centered on the chunk, in highlight text.
            p.setPen(palette().color(QPalette::HighlightedText));
            p.drawText(fill, Qt::AlignCenter, label);
        } else {
            // No room yet: just right of the chunk, normal text.
            int x = fill.left() + fill.width() + gap;
            if (x + tw > groove.right())
                x = groove.right() - tw;
            if (x < groove.left())
                x = groove.left();
            p.setPen(palette().color(QPalette::WindowText));
            p.drawText(QRect(x, groove.center().y() - th / 2, tw, th),
                       Qt::AlignLeft | Qt::AlignVCenter, label);
        }
    }
    p.restore();
}
