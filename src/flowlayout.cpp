#include "flowlayout.h"

#include <QApplication>
#include <QStyle>
#include <QWidget>

FlowLayout::FlowLayout(QWidget *parent, int margin, int hSpacing, int vSpacing)
    : QLayout(parent), m_hSpace(hSpacing), m_vSpace(vSpacing) {
    setContentsMargins(margin, margin, margin, margin);
}

FlowLayout::~FlowLayout() {
    QLayoutItem *item;
    while ((item = takeAt(0)))
        delete item;
}

void FlowLayout::clear() {
    QLayoutItem *item;
    while ((item = takeAt(0))) {
        delete item->widget();
        delete item;
    }
}

void FlowLayout::addItem(QLayoutItem *item) {
    m_items.append(item);
}

int FlowLayout::horizontalSpacing() const {
    if (m_hSpace >= 0)
        return m_hSpace;
    QWidget *pw = parentWidget();
    QStyle *st = pw ? pw->style() : QApplication::style();
    return st->pixelMetric(QStyle::PM_LayoutHorizontalSpacing, nullptr, pw);
}

int FlowLayout::verticalSpacing() const {
    if (m_vSpace >= 0)
        return m_vSpace;
    QWidget *pw = parentWidget();
    QStyle *st = pw ? pw->style() : QApplication::style();
    return st->pixelMetric(QStyle::PM_LayoutVerticalSpacing, nullptr, pw);
}

Qt::Orientations FlowLayout::expandingDirections() const {
    return {};
}

bool FlowLayout::hasHeightForWidth() const {
    return true;
}

int FlowLayout::heightForWidth(int width) const {
    return doLayout(QRect(0, 0, width, 0), true);
}

int FlowLayout::count() const {
    return m_items.size();
}

QLayoutItem *FlowLayout::itemAt(int index) const {
    return m_items.value(index);
}

QSize FlowLayout::minimumSize() const {
    QSize size;
    for (const QLayoutItem *item : m_items)
        size = size.expandedTo(item->minimumSize());
    const QMargins margins = contentsMargins();
    size += QSize(margins.left() + margins.right(), margins.top() + margins.bottom());
    return size;
}

void FlowLayout::setGeometry(const QRect &rect) {
    QLayout::setGeometry(rect);
    doLayout(rect, false);
}

QSize FlowLayout::sizeHint() const {
    return minimumSize();
}

QLayoutItem *FlowLayout::takeAt(int index) {
    if (index >= 0 && index < m_items.size())
        return m_items.takeAt(index);
    return nullptr;
}

int FlowLayout::doLayout(const QRect &rect, bool testOnly) const {
    int left, top, right, bottom;
    getContentsMargins(&left, &top, &right, &bottom);
    const QRect area = rect.adjusted(left, top, -right, -bottom);
    int x = area.x();
    int y = area.y();
    int lineHeight = 0;
    for (QLayoutItem *item : m_items) {
        const int spaceX = horizontalSpacing();
        const int spaceY = verticalSpacing();
        int nextX = x + item->sizeHint().width() + spaceX;
        if (nextX - spaceX > area.right() && lineHeight > 0) {
            x = area.x();
            y = y + lineHeight + spaceY;
            nextX = x + item->sizeHint().width() + spaceX;
            lineHeight = 0;
        }
        if (!testOnly)
            item->setGeometry(QRect(QPoint(x, y), item->sizeHint()));
        x = nextX;
        lineHeight = qMax(lineHeight, item->sizeHint().height());
    }
    return y + lineHeight - rect.y() + bottom;
}
