#include "theme.h"

#include <QApplication>
#include <QFile>
#include <QPalette>
#include <QStyle>
#include <QStyleFactory>

namespace Theme {

void applyDefault(QApplication &app, const QColor &accentIn) {
    // Never inherit the desktop environment's theme unless the user asks:
    // pin Fusion and paint our own dark palette over it.
    if (QStyleFactory::keys().contains(QStringLiteral("Fusion")))
        app.setStyle(QStringLiteral("Fusion"));

    const QColor accent = accentIn.isValid() ? accentIn : QColor(0x3a, 0x7e, 0xc2);
    const QColor accentHi = accent.lighter(135);
    const QString acc = accent.name();

    // KeePassXC-inspired dark scheme: slate surfaces, blue accent default.
    QPalette p;
    const QColor bg(0x23, 0x25, 0x2e);      // window
    const QColor base(0x1c, 0x1e, 0x26);    // text entry / view base
    const QColor alt(0x2a, 0x2d, 0x3a);     // alternate rows
    const QColor button(0x2e, 0x31, 0x3d);
    const QColor text(0xe0, 0xe2, 0xec);
    const QColor dim(0x9aa0b8);
    const QColor disabled(0x6a7089);
    const QColor border(0x3a, 0x3f, 0x55);

    p.setColor(QPalette::Window, bg);
    p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::Base, base);
    p.setColor(QPalette::AlternateBase, alt);
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::Button, button);
    p.setColor(QPalette::ButtonText, text);
    p.setColor(QPalette::BrightText, Qt::white);
    p.setColor(QPalette::ToolTipBase, alt);
    p.setColor(QPalette::ToolTipText, text);
    p.setColor(QPalette::Highlight, accent);
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::Link, accentHi);
    p.setColor(QPalette::LinkVisited, QColor(0xa7, 0x8b, 0xfa));
    p.setColor(QPalette::Mid, border);
    p.setColor(QPalette::Dark, base);
    p.setColor(QPalette::Light, QColor(0x3f, 0x44, 0x5c));
    p.setColor(QPalette::Shadow, Qt::black);
    p.setColor(QPalette::Disabled, QPalette::WindowText, disabled);
    p.setColor(QPalette::Disabled, QPalette::Text, disabled);
    p.setColor(QPalette::Disabled, QPalette::ButtonText, disabled);
    app.setPalette(p);
    app.setStyleSheet(
        QStringLiteral("QToolTip { color: #e0e2ec; background: #2a2d3a; "
                       "border: 1px solid #3a3f55; }"
                       "QListView::item:selected, QListWidget::item:selected { "
                       "background: %1; color: white; }"
                       "QSplitter::handle { background: #2a2f45; }"
                       "QScrollBar:vertical { background: transparent; width: 10px; }"
                       "QScrollBar::handle:vertical { background: #3a3f55; "
                       "border-radius: 4px; min-height: 20px; }"
                       "QLineEdit { selection-background-color: %1; }"
                       "QProgressBar::chunk { background: %1; }")
            .arg(acc));
    Q_UNUSED(dim);
}

bool applyStylesheetFile(QApplication &app, const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;
    // Applied on top of the built-in palette: custom Qt themes welcome.
    app.setStyleSheet(app.styleSheet() + u'\n' +
                      QString::fromUtf8(f.readAll()));
    return true;
}

void applyNative(QApplication &app) {
    // Opt back into the desktop environment's look: default style,
    // system palette, no custom stylesheet.
    app.setStyleSheet(QString());
    app.setPalette(app.style()->standardPalette());
}

} // namespace Theme
