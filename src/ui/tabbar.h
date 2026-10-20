#pragma once

// Tab bar with music-player-style marquee: tabs keep a sane width (text
// still elides for measurement), but overflowing titles slide instead of
// sitting cut off. Full title is always in the tooltip. Drag-reorder
// works: titles track tab moves, and painting falls back to the native
// style while a drag is in flight (the native style owns the dragged-tab
// pixmap and drop indicator).
#include <QTabBar>
#include <QMouseEvent>
#include <QString>
#include <QTimer>
#include <QVector>

class MarqueeTabBar : public QTabBar {
    Q_OBJECT
public:
    explicit MarqueeTabBar(QWidget *parent = nullptr);

    // Full (untrimmed) titles, parallel to tab indexes.
    void setFullTitle(int index, const QString &title);
    QString fullTitle(int index) const;

protected:
    void paintEvent(QPaintEvent *e) override;
    QSize tabSizeHint(int index) const override; // widen for multi-mark icons
    void tabInserted(int index) override;
    void tabRemoved(int index) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;

private slots:
    void onTick();
    void onTabMoved(int from, int to);

private:
    bool tabOverflows(int index) const;
    QTimer *m_timer;
    int m_offset = 0;
    QVector<QString> m_fullTitles;
    // Native-drag tracking: while a reorder drag is in flight the base
    // class paints (it owns the drag pixmap + drop indicator).
    bool m_pressed = false;
    bool m_dragging = false;
    QPoint m_pressPos;
};
