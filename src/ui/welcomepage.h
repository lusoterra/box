#pragma once

// VS Code-style welcome page: resume last session, recent archives,
// open-folder picker, and the "always load last session" toggle
// (mirrored in Preferences, either side can disable it).
#include <QWidget>

#include <QPixmap>

class QCheckBox;
class QLabel;
class QListWidget;
class QPushButton;
class QGraphicsOpacityEffect;

class WelcomePage : public QWidget {
    Q_OBJECT
public:
    explicit WelcomePage(QWidget *parent = nullptr);

    // history: most-recent-first archive roots; lastRoot highlighted.
    // Missing directories are skipped.
    void setRestore(bool on);
    // "Back to last tab" button: empty label hides it (nothing to resume).
    void setLastPlace(const QString &label);
    // Custom background mascot (home page only); empty path hides it.
    void setMascot(const QString &path);
    // Translucency (0.1-1.0) and size: height fraction of the page
    // (0.1-0.65), 1.0 = max height (full window height), 2.0 = full
    // width (window width, aspect kept, overflow cropped top).
    void setMascotOpacity(double opacity);
    void setMascotScale(double scale);
    // Right/bottom margin in px (few-px window-edge padding).
    void setMascotMargin(int px);

signals:
    void openDialogRequested();
    void browseTabRequested();
    void favTabRequested();
    void downloadsRequested();
    void resumeRequested();
    // Double-clicked the mascot: size cycled, persist it.
    void mascotScaleChanged(double scale);
    void restoreToggled(bool on);

protected:
    void resizeEvent(QResizeEvent *e) override;
    bool eventFilter(QObject *watched, QEvent *e) override;

private:
    void layoutMascot();

    QCheckBox *m_always;
    QPushButton *m_resume = nullptr;
    QLabel *m_mascot = nullptr;
    QGraphicsOpacityEffect *m_mascotDim = nullptr;
    QPixmap m_mascotFull;
    double m_mascotScale = 0.45; // height fraction (0.1-0.65), 1.0 = max
                                 // height, 2.0 = full width; cycles them
    int m_mascotMargin = 0;      // right/bottom padding in px
};
