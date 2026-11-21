// Informational status bar: every bundled theme must paint it darker
// than the window, with text that keeps WCAG AA contrast.
#include "theme.h"

#include <QApplication>
#include <QTemporaryDir>
#include <QtTest>
#include <algorithm>
#include <cmath>

namespace {
double channel(double v) {
    v /= 255.0;
    return v <= 0.03928 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
}

double contrast(const QColor &a, const QColor &b) {
    auto lum = [](const QColor &c) {
        return 0.2126 * channel(c.red()) + 0.7152 * channel(c.green()) +
               0.0722 * channel(c.blue());
    };
    const double la = lum(a);
    const double lb = lum(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}
} // namespace

class TestTheme : public QObject {
    Q_OBJECT
private slots:
    void statusBarIsDarkerWithReadableText();
    void statusBarTextFallsBackOnLowContrast();
};

void TestTheme::statusBarIsDarkerWithReadableText() {
    const QList<Theme::ThemeInfo> themes = Theme::availableThemes();
    QVERIFY(!themes.isEmpty());
    for (const Theme::ThemeInfo &info : themes) {
        const Theme::PaletteColors c = Theme::colorsFor(info.key, {});
        const QColor bg = Theme::statusBarBackground(c.window, c.dark);
        const QColor fg = Theme::statusBarText(bg, c.text);
        QVERIFY2(bg.lightness() < c.window.lightness(),
                 qPrintable(info.key));
        const double ratio = contrast(bg, fg);
        QVERIFY2(ratio >= 4.5,
                 qPrintable(QStringLiteral("%1: %2:1")
                                .arg(info.key)
                                .arg(ratio, 0, 'f', 2)));
    }
}

void TestTheme::statusBarTextFallsBackOnLowContrast() {
    const QColor bg(0x80, 0x80, 0x80);
    const QColor fg = Theme::statusBarText(bg, bg);
    QVERIFY(contrast(bg, fg) >= 4.5);
}

int main(int argc, char **argv) {
    // Keep the user's ~/.config/box/themes out of the bundled-theme check.
    QTemporaryDir config;
    qputenv("XDG_CONFIG_HOME", config.path().toUtf8());
    QApplication app(argc, argv);
    TestTheme test;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&test, argc, argv);
}

#include "test_theme.moc"
