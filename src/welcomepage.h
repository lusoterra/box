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

class WelcomePage : public QWidget {
    Q_OBJECT
public:
    explicit WelcomePage(QWidget *parent = nullptr);

    // history: most-recent-first archive roots; lastRoot highlighted.
    // Missing directories are skipped.
    void setRestore(bool on);
    // Custom background mascot (home page only); empty path hides it.
    void setMascot(const QString &path);

signals:
    void openDialogRequested();
    void browseTabRequested();
    void favTabRequested();
    void downloadsRequested();
    void restoreToggled(bool on);

protected:
    void resizeEvent(QResizeEvent *e) override;

private:
    void layoutMascot();

    QCheckBox *m_always;
    QLabel *m_mascot = nullptr;
    QPixmap m_mascotFull;
};
