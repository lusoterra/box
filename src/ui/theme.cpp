#include "theme.h"

#include "uicons.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QPalette>
#include <QSettings>
#include <QStandardPaths>
#include <algorithm>
#include <QStyle>
#include <QStyleFactory>

namespace Theme {
namespace {

QColor hex(const char *s) {
    return QColor(QString::fromLatin1(s));
}

// The settings default accent is the built-in blue; only override a named
// theme's published accent when the user actually picked another color.
QColor themedAccent(const QColor &in, const QColor &themeDefault) {
    if (!in.isValid() ||
        in.name().compare(QStringLiteral("#3a7ec2"), Qt::CaseInsensitive) == 0)
        return themeDefault;
    return in;
}

PaletteColors builtinDark(const QColor &accentIn) {
    const QColor accent =
        accentIn.isValid() ? accentIn : QColor(0x3a, 0x7e, 0xc2);
    return {
        QColor(0x23, 0x25, 0x2e), // window
        QColor(0x1c, 0x1e, 0x26), // base
        QColor(0x2a, 0x2d, 0x3a), // alternate
        QColor(0x2e, 0x31, 0x3d), // button
        QColor(0xe0, 0xe2, 0xec), // text
        QColor(0x9a, 0xa0, 0xb8), // dim
        QColor(0x6a, 0x70, 0x89), // disabled
        QColor(0x3a, 0x3f, 0x55), // border
        accent,
        true,
    };
}

PaletteColors keepassxcDark(const QColor &accentIn) {
    // Palette from KeePassXC src/gui/styles/dark/DarkStyle.cpp, using the
    // user's accent for selection (their green is the highlight default).
    return {
        hex("#3B3B3D"), hex("#27272A"), hex("#2C2C30"), hex("#28282B"),
        hex("#CACBCE"), hex("#A8A9AD"), hex("#707070"), hex("#414145"),
        themedAccent(accentIn, hex("#2D532D")), true,
    };
}

PaletteColors keepassxcLight(const QColor &accentIn) {
    // Palette from KeePassXC src/gui/styles/light/LightStyle.cpp.
    return {
        hex("#F7F7F7"), hex("#F9F9F9"), hex("#ECF3E8"), hex("#D4D5DD"),
        hex("#1D1D20"), hex("#71727D"), hex("#8C8C92"), hex("#C9C9CF"),
        themedAccent(accentIn, hex("#507F1F")), false,
    };
}

PaletteColors genericLight(const QColor &accentIn) {
    return {
        hex("#F5F6F8"), hex("#FFFFFF"), hex("#EEF1F6"), hex("#E6E8ED"),
        hex("#20242E"), hex("#4B5567"), hex("#7B8496"), hex("#B8C1D1"),
        themedAccent(accentIn, hex("#2F6FBF")), false,
    };
}

PaletteColors nord(const QColor &accentIn) {
    return {
        hex("#2E3440"), hex("#2E3440"), hex("#3B4252"), hex("#434C5E"),
        hex("#ECEFF4"), hex("#D8DEE9"), hex("#4C566A"), hex("#4C566A"),
        themedAccent(accentIn, hex("#88C0D0")), true,
    };
}

// Catppuccin official palette (catppuccin/palette palette.json 1.8.0).
PaletteColors catppuccin(const QString &flavor, const QColor &accentIn) {
    struct C {
        const char *base, *mantle, *crust, *surface0, *surface1, *surface2,
            *text, *subtext0, *overlay0, *blue;
        bool dark;
    };
    static const QMap<QString, C> table = {
        {QStringLiteral("latte"),
         {"#eff1f5", "#e6e9ef", "#dce0e8", "#ccd0da", "#bcc0cc",
          "#acb0be", "#4c4f69", "#6c6f85", "#9ca0b0", "#1e66f5", false}},
        {QStringLiteral("frappe"),
         {"#303446", "#292c3c", "#232634", "#414559", "#51576d",
          "#626880", "#c6d0f5", "#a5adce", "#737994", "#8caaee", true}},
        {QStringLiteral("macchiato"),
         {"#24273a", "#1e2030", "#181926", "#363a4f", "#494d64",
          "#5b6078", "#cad3f5", "#a5adcb", "#6e738d", "#8aadf4", true}},
        {QStringLiteral("mocha"),
         {"#1e1e2e", "#181825", "#11111b", "#313244", "#45475a",
          "#585b70", "#cdd6f4", "#a6adc8", "#6c7086", "#89b4fa", true}},
    };
    const C c = table.value(flavor, table.value(QStringLiteral("mocha")));
    return {
        hex(c.base), hex(c.mantle), hex(c.surface0), hex(c.surface0),
        hex(c.text), hex(c.subtext0), hex(c.overlay0), hex(c.surface1),
        themedAccent(accentIn, hex(c.blue)), c.dark,
    };
}

PaletteColors dracula(const QColor &accentIn) {
    return {
        hex("#282A36"), hex("#21222C"), hex("#44475A"), hex("#44475A"),
        hex("#F8F8F2"), hex("#6272A4"), hex("#6272A4"), hex("#44475A"),
        themedAccent(accentIn, hex("#BD93F9")), true,
    };
}

PaletteColors atomOneDark(const QColor &accentIn) {
    return {
        hex("#282C34"), hex("#21252B"), hex("#2C313A"), hex("#353B45"),
        hex("#ABB2BF"), hex("#5C6370"), hex("#5C6370"), hex("#3E4451"),
        themedAccent(accentIn, hex("#61AFEF")), true,
    };
}

PaletteColors tokyoNight(const QColor &accentIn) {
    // tokyonight.nvim night palette (folke/tokyonight.nvim).
    return {
        hex("#1A1B26"), hex("#16161E"), hex("#1F2335"), hex("#292E42"),
        hex("#C0CAF5"), hex("#565F89"), hex("#565F89"), hex("#3B4261"),
        themedAccent(accentIn, hex("#7AA2F7")), true,
    };
}

void applyColors(QApplication &app, const PaletteColors &c) {
    if (QStyleFactory::keys().contains(QStringLiteral("Fusion")))
        app.setStyle(QStringLiteral("Fusion"));

    QPalette p;
    p.setColor(QPalette::Window, c.window);
    p.setColor(QPalette::WindowText, c.text);
    p.setColor(QPalette::Base, c.base);
    p.setColor(QPalette::AlternateBase, c.alternate);
    p.setColor(QPalette::Text, c.text);
    p.setColor(QPalette::Button, c.button);
    p.setColor(QPalette::ButtonText, c.text);
    p.setColor(QPalette::BrightText, c.dark ? Qt::white : Qt::black);
    p.setColor(QPalette::ToolTipBase, c.alternate);
    p.setColor(QPalette::ToolTipText, c.text);
    const bool lightAccent = c.accent.lightness() > 160;
    p.setColor(QPalette::Highlight, c.accent);
    p.setColor(QPalette::HighlightedText,
               lightAccent ? Qt::black : Qt::white);
    p.setColor(QPalette::Link, c.accent.lighter(115));
    p.setColor(QPalette::LinkVisited, hex("#a78bfa"));
    p.setColor(QPalette::Mid, c.border);
    p.setColor(QPalette::Midlight, c.alternate);
    p.setColor(QPalette::Dark, c.base);
    p.setColor(QPalette::Light, c.dark ? c.border.lighter(115)
                                       : c.border.lighter(108));
    p.setColor(QPalette::Shadow, c.dark ? Qt::black : c.border.darker(130));
    p.setColor(QPalette::PlaceholderText, c.dim);
    p.setColor(QPalette::Disabled, QPalette::WindowText, c.disabled);
    p.setColor(QPalette::Disabled, QPalette::Text, c.disabled);
    p.setColor(QPalette::Disabled, QPalette::ButtonText, c.disabled);
    p.setColor(QPalette::Disabled, QPalette::PlaceholderText, c.disabled);
    app.setPalette(p);
    UiIcons::setNormalColor(c.text);

    const QString acc = c.accent.name();
    const QString sel = lightAccent ? QStringLiteral("black")
                                    : QStringLiteral("white");
    app.setStyleSheet(
        QStringLiteral("QToolTip { color: %1; background: %2; "
                       "border: 1px solid %3; }"
                       "QListView::item:selected, QListWidget::item:selected { "
                       "background: %4; color: %5; }"
                       "QSplitter::handle { background: %3; }"
                       "QScrollBar:vertical { background: transparent; width: 10px; }"
                       "QScrollBar::handle:vertical { background: %6; "
                       "border-radius: 4px; min-height: 20px; }"
                       "QLineEdit { selection-background-color: %4; }"
                       "QProgressBar::chunk { background: %4; }")
            .arg(c.text.name(), c.alternate.name(), c.border.name(), acc, sel,
                 c.border.name()));
}

} // namespace

namespace {

QString userThemeDir() {
    return QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) +
           QStringLiteral("/box/themes");
}

bool safeThemeKey(const QString &key) {
    return !key.isEmpty() && key == QFileInfo(key).fileName() &&
           !key.contains(u'/') && !key.contains(u'\\') &&
           key != QStringLiteral(".") && key != QStringLiteral("..");
}

QString themePathForKey(const QString &key) {
    if (!safeThemeKey(key))
        return {};
    const QString user = userThemeDir() + u'/' + key + QStringLiteral(".ini");
    if (QFile::exists(user))
        return user;
    const QString builtin =
        QStringLiteral(":/themes/") + key + QStringLiteral(".ini");
    return QFile::exists(builtin) ? builtin : QString();
}

PaletteColors loadThemeFile(const QString &path, const QColor &accentIn,
                            bool *ok) {
    PaletteColors c = builtinDark(accentIn);
    *ok = false;
    QSettings s(path, QSettings::IniFormat);
    s.beginGroup(QStringLiteral("theme"));
    if (!s.contains(QStringLiteral("window")))
        return c;
    auto read = [&](const char *key, const QColor &fallback) {
        const QColor v(s.value(QLatin1String(key)).toString());
        return v.isValid() ? v : fallback;
    };
    c.window = read("window", c.window);
    c.base = read("base", c.base);
    c.alternate = read("alternate", c.alternate);
    c.button = read("button", c.button);
    c.text = read("text", c.text);
    c.dim = read("dim", c.dim);
    c.disabled = read("disabled", c.disabled);
    c.border = read("border", c.border);
    c.dark = s.value(QStringLiteral("dark"), c.dark).toBool();
    const QColor fileAccent(s.value(QStringLiteral("accent")).toString());
    if (fileAccent.isValid())
        c.accent = themedAccent(accentIn, fileAccent);
    *ok = true;
    return c;
}

} // namespace

QList<ThemeInfo> availableThemes() {
    QMap<QString, ThemeInfo> byKey;
    auto scan = [&](const QString &dir, bool user) {
        const QDir d(dir);
        for (const QString &f :
             d.entryList({QStringLiteral("*.ini")}, QDir::Files)) {
            const QString path = d.filePath(f);
            QSettings s(path, QSettings::IniFormat);
            s.beginGroup(QStringLiteral("theme"));
            const QString key = f.left(f.size() - 4);
            ThemeInfo info;
            info.key = user ? key : key; // user files may override built-ins
            info.label = s.value(QStringLiteral("name"), key).toString();
            info.dark = s.value(QStringLiteral("dark"), true).toBool();
            info.order = s.value(QStringLiteral("order"), user ? 1000 : 100)
                             .toInt();
            if (user)
                info.order += 1000; // drop-ins sort after the shipped set
            byKey.insert(key, info);
        }
    };
    scan(QStringLiteral(":/themes"), false);
    scan(userThemeDir(), true);
    QList<ThemeInfo> out = byKey.values();
    if (out.isEmpty()) {
        // Resource listing failed (stripped build): keep the shipped keys
        // selectable instead of showing only "System native".
        const QList<QPair<QString, QString>> fallback = {
            {QStringLiteral("builtin"), QStringLiteral("Box dark")},
            {QStringLiteral("keepassxc-dark"), QStringLiteral("KeePassXC dark")},
            {QStringLiteral("keepassxc-light"), QStringLiteral("KeePassXC light")},
            {QStringLiteral("light"), QStringLiteral("Light")},
            {QStringLiteral("nord"), QStringLiteral("Nord")},
            {QStringLiteral("catppuccin-latte"), QStringLiteral("Catppuccin Latte")},
            {QStringLiteral("catppuccin-frappe"), QStringLiteral("Catppuccin Frappé")},
            {QStringLiteral("catppuccin-macchiato"), QStringLiteral("Catppuccin Macchiato")},
            {QStringLiteral("catppuccin-mocha"), QStringLiteral("Catppuccin Mocha")},
            {QStringLiteral("dracula"), QStringLiteral("Dracula")},
            {QStringLiteral("atom-one-dark"), QStringLiteral("Atom One Dark")},
            {QStringLiteral("tokyo-night"), QStringLiteral("Tokyo Night")},
        };
        int order = 0;
        for (const auto &[key, label] : fallback)
            out.append({key, label, true, order++});
    }
    std::sort(out.begin(), out.end(), [](const ThemeInfo &a,
                                         const ThemeInfo &b) {
        if (a.order != b.order)
            return a.order < b.order;
        return a.label.localeAwareCompare(b.label) < 0;
    });
    return out;
}

PaletteColors colorsFor(const QString &mode, const QColor &accent) {
    // Themes are data files now: built-ins live in :/themes/*.ini, user
    // drop-ins in ~/.config/box/themes/*.ini (user overrides a built-in
    // with the same file name). Unknown/legacy keys fall back to the dark
    // default so an old settings.ini never breaks startup.
    bool ok = false;
    const QString path = themePathForKey(mode);
    if (!path.isEmpty()) {
        PaletteColors c = loadThemeFile(path, accent, &ok);
        if (ok)
            return c;
    }
    return builtinDark(accent);
}

void applyTheme(QApplication &app, const QString &mode,
                const QColor &accent) {
    if (mode == QStringLiteral("native")) {
        applyNative(app);
        return;
    }
    applyColors(app, colorsFor(mode, accent));
}

void applyDefault(QApplication &app, const QColor &accentIn) {
    applyColors(app, builtinDark(accentIn));
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
    UiIcons::setNormalColor(app.palette().color(QPalette::WindowText));
}

} // namespace Theme
