#pragma once

// Wrapping layout for tag buttons / filter pills (classic Qt FlowLayout:
// items flow left-to-right and wrap, all left-aligned).
#include <QLayout>
#include <QList>
#include <QStyle>

class FlowLayout : public QLayout {
public:
    explicit FlowLayout(QWidget *parent = nullptr, int margin = 0, int hSpacing = -1,
                        int vSpacing = -1);
    ~FlowLayout() override;

    void addItem(QLayoutItem *item) override;
    int horizontalSpacing() const;
    int verticalSpacing() const;
    Qt::Orientations expandingDirections() const override;
    bool hasHeightForWidth() const override;
    int heightForWidth(int width) const override;
    int count() const override;
    QLayoutItem *itemAt(int index) const override;
    QSize minimumSize() const override;
    void setGeometry(const QRect &rect) override;
    QSize sizeHint() const override;
    QLayoutItem *takeAt(int index) override;

    void clear();

private:
    int doLayout(const QRect &rect, bool testOnly) const;
    QList<QLayoutItem *> m_items;
    int m_hSpace;
    int m_vSpace;
};
