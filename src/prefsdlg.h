#pragma once

#include "settings.h"

#include <QDialog>
#include <QLabel>
#include <QPlainTextEdit>

class QCheckBox;
class QComboBox;
class QLineEdit;
class QPushButton;
class QSlider;

// Preferences dialog editing a copy of AppSettings. On accept(), use
// settings() for the result; MainWindow applies + saves.
class PrefsDialog : public QDialog {
    Q_OBJECT
public:
    explicit PrefsDialog(const AppSettings &current, QWidget *parent = nullptr);
    AppSettings settings() const { return m_result; }

private:
    void harvest();
    void setSwatch(const QColor &c);

    AppSettings m_result;
    QCheckBox *m_restore;
    QCheckBox *m_classify;
    QLineEdit *m_gelUser;
    QLineEdit *m_gelKey;
    QLabel *m_gelStatus;
    QCheckBox *m_indexCache;
    QCheckBox *m_thumbCache;
    QComboBox *m_thumbSize;
    QComboBox *m_themeMode;
    QPushButton *m_accentBtn;
    QColor m_accent;
    QLineEdit *m_stylesheet;
    QComboBox *m_aspect;
    int m_customNum = 1;
    int m_customDen = 1;
    QPlainTextEdit *m_explicit;
    QLabel *m_explicitCount;
    QCheckBox *m_muted;
    QCheckBox *m_loop;
    QSlider *m_volume;
    QComboBox *m_maxSide;
};
