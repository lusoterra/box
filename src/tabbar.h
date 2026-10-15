#pragma once

// Tab bar with music-player-style marquee: tabs keep a sane width (text
// still elides for measurement), but overflowing titles slide instead of
// sitting cut off. Full title is always in the tooltip.
#include <QTabBar>
#include <QHash>
#include <QString>
#include <QTimer>

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

private slots:
    void onTick();

private:
    bool tabOverflows(int index) const;
    QTimer *m_timer;
    int m_offset = 0;
    QHash<int, QString> m_titles;
};
