#pragma once

// Tab bar with music-player-style marquee: tabs keep a sane width (text
// still elides for measurement), but overflowing titles slide instead of
// sitting cut off. Full title is always in the tooltip. Drag-reorder
// works browser-style: a raised overlay shows a grab of the pressed tab
// that slides horizontally in its row while the strip reorders underneath.
#include <QTabBar>
#include <QMouseEvent>
#include <QPixmap>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QVector>

class QLabel;

class MarqueeTabBar : public QTabBar {
    Q_OBJECT
public:
    explicit MarqueeTabBar(QWidget *parent = nullptr);

    // Full (untrimmed) titles, parallel to tab indexes.
    void setFullTitle(int index, const QString &title);
    QString fullTitle(int index) const;
    // A reorder drag is in flight. Reorder emits currentChanged at every
    // step; the owner must not treat that as "user selected another tab".
    bool isDragging() const { return m_dragging; }

protected:
    void paintEvent(QPaintEvent *) override;
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
    // Drag ghost: a raised window child showing a grab of the pressed tab.
    void showGhost();
    void moveGhost();
    QTimer *m_timer;
    int m_offset = 0;
    QVector<QString> m_fullTitles;
    // Native-drag tracking: while a reorder drag is in flight the base
    // class paints (it owns the drag pixmap + drop indicator).
    bool m_pressed = false;
    bool m_dragging = false;
    QPoint m_pressPos;
    // Floating copy of the dragged tab. It lives in its own raised widget
    // (see showGhost) so the strip's close buttons cannot paint over it.
    // m_dragPos.y() stays pinned at the press y: horizontal-only slide.
    QPixmap m_dragPixmap;
    QPoint m_dragHotspot;
    QPoint m_dragPos;
    QPointer<QLabel> m_ghost;
};
