#pragma once

#include <QColor>
#include <QList>
#include <QString>

class QApplication;

// Theming: Fusion style forced by default (no desktop-environment dependence,
// like KeePassXC), built-in palettes (dark + light, Nord, Catppuccin,
// Dracula, Atom One Dark, KeePassXC, Tokyo Night), a configurable accent
// color, and optional user-supplied .qss stylesheets applied on top.
namespace Theme {
struct PaletteColors {
    QColor window;
    QColor base;
    QColor alternate;
    QColor button;
    QColor text;
    QColor dim;
    QColor disabled;
    QColor border;
    QColor accent;
    bool dark = true;
};

struct ThemeInfo {
    QString key;    // settings.ini theme/mode value
    QString label;  // combo text
    bool dark = true;
    int order = 1000;
};

// Built-in `:/themes/*.ini` plus user drop-ins from
// ~/.config/box/themes/*.ini, sorted for the Preferences combo.
QList<ThemeInfo> availableThemes();
// Palette for a settings.ini theme key (file stem, e.g. "nord").
PaletteColors colorsFor(const QString &mode, const QColor &accent);
// Apply one of the built-in palettes. Unknown/native keys fall back to the
// default dark theme ("native" is handled by applyNative()).
void applyTheme(QApplication &app, const QString &mode,
                const QColor &accent = {});
void applyDefault(QApplication &app, const QColor &accent = {}); // builtin
bool applyStylesheetFile(QApplication &app, const QString &path);
void applyNative(QApplication &app); // opt back into the desktop theme
} // namespace Theme
