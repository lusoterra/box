#pragma once

#include <QColor>
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

// Palette for a settings.ini theme key ("builtin", "nord", "dracula", …).
PaletteColors colorsFor(const QString &mode, const QColor &accent);
// Apply one of the built-in palettes. Unknown/native keys fall back to the
// default dark theme ("native" is handled by applyNative()).
void applyTheme(QApplication &app, const QString &mode,
                const QColor &accent = {});
void applyDefault(QApplication &app, const QColor &accent = {}); // builtin
bool applyStylesheetFile(QApplication &app, const QString &path);
void applyNative(QApplication &app); // opt back into the desktop theme
} // namespace Theme
