#include "mainwindow.h"
#include "settings.h"
#include "theme.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QIcon>

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
} // namespace

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("booru-explorer"));
    app.setOrganizationName(QStringLiteral("booru-explorer"));
    app.setWindowIcon(QIcon(QStringLiteral(":/icons/app.png")));

    const AppSettings settings = loadSettings();
    // Theme before the window exists so first paint is already correct.
    // CLI flags win over the config file for this launch.
    QCommandLineParser cli;
    cli.setApplicationDescription(
        QStringLiteral("Read-only local booru viewer (gallery-dl + LOSN metadata)"));
    cli.addHelpOption();
    cli.addPositionalArgument(QStringLiteral("root"), QStringLiteral("Folder to scan."));
    QCommandLineOption styleOpt(QStringLiteral("stylesheet"),
                                QStringLiteral("Apply a custom .qss Qt stylesheet on top "
                                               "of the built-in dark theme."),
                                QStringLiteral("file.qss"));
    cli.addOption(styleOpt);
    QCommandLineOption nativeOpt(QStringLiteral("native-theme"),
                                 QStringLiteral("Use the desktop environment's Qt theme "
                                                "instead of the built-in dark theme."));
    cli.addOption(nativeOpt);
    cli.process(app);
    applyStartupTheme(app, settings, cli.value(styleOpt), cli.isSet(nativeOpt));

    const QStringList pos = cli.positionalArguments();
    const bool rootExplicit = !pos.isEmpty();
    const QString root = rootExplicit ? pos.value(0) : QDir::currentPath();

    MainWindow w(root, rootExplicit);
    w.show();
    return app.exec();
}
