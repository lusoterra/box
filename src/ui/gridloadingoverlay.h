#pragma once

// A veil over the thumbnail grid, shown the instant a tab is activated.
// The click paints a busy bar + label synchronously, before the tab's DB
// decode, scan or search starts, so a slow switch never looks ignored.
// The owning window hides it again when the target tab's content lands.
#include <QWidget>

class QLabel;
class CenterProgressBar;

class GridLoadingOverlay : public QWidget {
    Q_OBJECT
public:
    explicit GridLoadingOverlay(QWidget *parent = nullptr);
    // Show the busy state now: sets the label, sizes to the viewport,
    // raises and repaints synchronously (no waiting for the event loop).
    void showFor(const QString &what);

protected:
    bool eventFilter(QObject *watched, QEvent *e) override;
    void paintEvent(QPaintEvent *e) override;

private:
    CenterProgressBar *m_bar = nullptr;
    QLabel *m_label = nullptr;
};
