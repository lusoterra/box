#pragma once

#include <QColor>
#include <QString>

class QApplication;

// Theming: Fusion style forced by default (no desktop-environment dependence,
// like KeePassXC), a built-in KeePassXC-inspired dark palette with a
// configurable accent color, and optional user-supplied .qss stylesheets
// applied on top.
namespace Theme {
void applyDefault(QApplication &app, const QColor &accent = {});
bool applyStylesheetFile(QApplication &app, const QString &path);
void applyNative(QApplication &app); // opt back into the desktop theme
} // namespace Theme
