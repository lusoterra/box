#include "mainwindow.h"
#include "settings.h"
#include "theme.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QGuiApplication>
#include <QIcon>
#include <QLibraryInfo>
#include <QLockFile>
#include <QMessageBox>
#include <QLocale>
#include <QTranslator>

namespace {
void applyStartupTheme(QApplication &app, const AppSettings &s,
                       const QString &cliStylesheet, bool cliNative) {
    if (cliNative || s.themeMode == QStringLiteral("native")) {
        Theme::applyNative(app);
    } else {
        Theme::applyDefault(app, QColor(s.accent));
    }
    if (!cliStylesheet.isEmpty())
        Theme::applyStylesheetFile(app, cliStylesheet);
    else if (!cliNative && s.themeMode != QStringLiteral("native") &&
             !s.stylesheet.isEmpty())
        Theme::applyStylesheetFile(app, s.stylesheet);
}
// CLI startup errors: stderr always; a popup too, unless headless
// (an offscreen message box would hang forever with nobody to dismiss it).
int cliError(const QString &msg) {
    fputs(qPrintable(msg + u'\n'), stderr);
    if (QGuiApplication::platformName() != QStringLiteral("offscreen"))
        QMessageBox::warning(nullptr, QStringLiteral("booru-explorer"), msg);
    return 1;
}
} // namespace

int main(int argc, char *argv[]) {
    // Software WebGL/WebGPU fallback for the cookie-capture browser:
    // Cloudflare's challenges compute in WebGL/WebGPU, and since ~M128
    // Chromium offers NO software fallback unless this flag is present.
    // Machines whose GPU process fails (VMs, Xvfb, flaky Wayland/NVIDIA
    // drivers — note the VDPAU/WebGPU errors in terminal output) would
    // otherwise retry the challenge forever while a normal browser, on
    // hardware GL, passes. Hardware path untouched; user's own flags kept.
    {
        QByteArray flags = qgetenv("QTWEBENGINE_CHROMIUM_FLAGS");
        if (!flags.contains("--enable-unsafe-swiftshader"))
            flags += " --enable-unsafe-swiftshader";
        qputenv("QTWEBENGINE_CHROMIUM_FLAGS", flags.trimmed());
    }
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("booru-explorer"));
    // No organization name: with org == app the platform cache dir nests
    // twice (~/.cache/booru-explorer/booru-explorer). Nothing depends on
    // the org name (all paths are explicit).
    app.setWindowIcon(QIcon(QStringLiteral(":/icons/app.png")));

    // i18n: translators add translations/booru-explorer_<locale>.ts and
    // the matching .qm embeds + loads automatically from :/i18n.
    // Preferences → General can pin a locale; otherwise the system wins.
    static QTranslator appTranslator;
    const QString forcedLang = loadSettings().language;
    QStringList locales;
    if (!forcedLang.isEmpty())
        locales << forcedLang << forcedLang.split(u'_').first();
    locales << QLocale::system().name()
            << QLocale::system().bcp47Name().split(u'-').first();
    for (const QString &loc : locales) {
        if (!loc.isEmpty() && loc != QStringLiteral("en") &&
            appTranslator.load(QStringLiteral("booru-explorer_") + loc,
                               QStringLiteral(":/i18n"))) {
            app.installTranslator(&appTranslator);
            break;
        }
    }
    static QTranslator qtTranslator;
    for (const QString &loc : locales) {
        if (!loc.isEmpty() &&
            qtTranslator.load(QStringLiteral("qt_") + loc,
                              QLibraryInfo::path(QLibraryInfo::TranslationsPath))) {
            app.installTranslator(&qtTranslator);
            break;
        }
    }

    // CLI flags win over the config file for this launch.
    QCommandLineParser cli;
    cli.setApplicationDescription(
        QCoreApplication::translate("CommandLine",
                                    "Booru client for local archives and online boorus"));
    cli.addHelpOption();
    cli.addPositionalArgument(QStringLiteral("root"),
                              QCoreApplication::translate("CommandLine", "Folder to scan."));
    QCommandLineOption styleOpt(QStringLiteral("stylesheet"),
                                QCoreApplication::translate(
                                    "CommandLine", "Apply a custom .qss Qt stylesheet on top "
                                                   "of the built-in dark theme."),
                                QStringLiteral("file.qss"));
    cli.addOption(styleOpt);
    QCommandLineOption nativeOpt(QStringLiteral("native-theme"),
                                 QCoreApplication::translate(
                                     "CommandLine", "Use the desktop environment's Qt theme "
                                                    "instead of the built-in dark theme."));
    cli.addOption(nativeOpt);
    cli.process(app);

    // Single instance: a second copy stops here instead of risking
    // the database.
    QDir().mkpath(appDataDir());
    static QLockFile appLock(appDataDir() + QStringLiteral("/session.lock"));
    if (!appLock.tryLock()) {
        return cliError(QCoreApplication::translate(
            "CommandLine", "booru-explorer is already running."));
    }
    // Theme before the window exists so first paint is already correct.
    const AppSettings settings = loadSettings();
    // Custom data dir (databases + downloads): make sure it exists too.
    // The global lock above stays at the default location so two
    // instances can never share settings.ini either.
    QDir().mkpath(effectiveDataDir(settings));
    applyStartupTheme(app, settings, cli.value(styleOpt), cli.isSet(nativeOpt));

    const QStringList pos = cli.positionalArguments();
    const bool rootExplicit = !pos.isEmpty();
    const QString root = rootExplicit ? pos.value(0) : QDir::currentPath();

    MainWindow w(root, rootExplicit);
    w.show();
    return app.exec();
}
