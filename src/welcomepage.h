#pragma once

// VS Code-style welcome page: resume last session, recent archives,
// open-folder picker, and the "always load last session" toggle
// (mirrored in Preferences, either side can disable it).
#include <QWidget>

class QCheckBox;
class QListWidget;
class QPushButton;

class WelcomePage : public QWidget {
    Q_OBJECT
public:
    explicit WelcomePage(QWidget *parent = nullptr);

    // history: most-recent-first archive roots; lastRoot highlighted.
    // Missing directories are skipped.
    void setHistory(const QStringList &history, const QString &lastRoot);
    void setRestore(bool on);

signals:
    void openDialogRequested();
    void openPathRequested(const QString &path);
    void restoreToggled(bool on);

private:
    QPushButton *m_resume;
    QListWidget *m_recent;
    QCheckBox *m_always;
    QString m_lastRoot;
};
