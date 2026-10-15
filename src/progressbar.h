#pragma once

// QProgressBar with readable % placement: the label sits in the middle
// of the filled chunk while the chunk is wide enough to hold it, and
// parks just right of the chunk until then (instead of the style's
// fixed alignment, which strands tiny labels half-clipped at the edge).
#include <QProgressBar>

class CenterProgressBar : public QProgressBar {
    Q_OBJECT
public:
    using QProgressBar::QProgressBar;

protected:
    void paintEvent(QPaintEvent *e) override;
};
