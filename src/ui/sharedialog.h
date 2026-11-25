#pragma once

// Per-start source picker for the read-only LAN sharing server.
#include <QDialog>
#include <QString>
#include <QStringList>

class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;

struct ShareOptions {
    bool hydrus = false;
    bool downloads = false;
    bool favourites = false;
    bool localFolder = false;
    QString localFolderPath;
    QString bindAddress = QStringLiteral("0.0.0.0");
    int port = 7071;
};

class ShareDialog : public QDialog {
    Q_OBJECT
public:
    enum class Action { None, Start, Stop, Refresh };

    ShareDialog(const ShareOptions &current, bool hydrusAvailable,
                int hydrusCount, bool running, int postCount,
                const QStringList &urls, QWidget *parent = nullptr);

    ShareOptions options() const;
    Action action() const { return m_action; }

private:
    void updateStartState();
    ShareOptions m_current;
    Action m_action = Action::None;

    QCheckBox *m_hydrus = nullptr;
    QCheckBox *m_downloads = nullptr;
    QCheckBox *m_favourites = nullptr;
    QCheckBox *m_local = nullptr;
    QLineEdit *m_folder = nullptr;
    QPushButton *m_browse = nullptr;
    QLineEdit *m_bind = nullptr;
    QSpinBox *m_port = nullptr;
    QLabel *m_status = nullptr;
    QLineEdit *m_url = nullptr;
    QPushButton *m_copyUrl = nullptr;
    QPushButton *m_start = nullptr;
    QPushButton *m_refresh = nullptr;
    QPushButton *m_stop = nullptr;
};
