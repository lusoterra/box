#include "settings.h"

#include <QDir>
#include <QSettings>
#include <QStandardPaths>

QString settingsFilePath() {
    return QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) +
           QStringLiteral("/booru-explorer/settings.ini");
}

QString appCacheDir() {
    return QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
}

namespace {
QSettings open() {
    return QSettings(settingsFilePath(), QSettings::IniFormat);
}
} // namespace

AppSettings loadSettings() {
    AppSettings s;
    QSettings c = open();
    s.indexCache = c.value(QStringLiteral("cache/index"), s.indexCache).toBool();
    s.thumbCache = c.value(QStringLiteral("cache/thumbs"), s.thumbCache).toBool();
    s.thumbLongSide = c.value(QStringLiteral("cache/thumbSize"), s.thumbLongSide).toInt();
    s.lastRoot = c.value(QStringLiteral("library/root")).toString();
    s.history = c.value(QStringLiteral("library/history")).toStringList();
    s.restoreSession =
        c.value(QStringLiteral("general/restoreSession"), s.restoreSession).toBool();
    s.classifyTags =
        c.value(QStringLiteral("classify/enabled"), s.classifyTags).toBool();
    s.gelbooruUser = c.value(QStringLiteral("gelbooru/user")).toString();
    s.gelbooruKey = c.value(QStringLiteral("gelbooru/key")).toString();
    if (c.contains(QStringLiteral("explicit/tags")))
        s.explicitTags =
            normalizeExplicitTags(c.value(QStringLiteral("explicit/tags")).toStringList());
    s.themeMode = c.value(QStringLiteral("theme/mode"), s.themeMode).toString();
    s.accent = c.value(QStringLiteral("theme/accent"), s.accent).toString();
    s.stylesheet = c.value(QStringLiteral("theme/stylesheet")).toString();
    s.aspectNum = c.value(QStringLiteral("grid/aspectNum"), s.aspectNum).toInt();
    s.aspectDen = c.value(QStringLiteral("grid/aspectDen"), s.aspectDen).toInt();
    if (s.aspectNum < 1 || s.aspectDen < 1) {
        s.aspectNum = 1;
        s.aspectDen = 1;
    }
    s.videoMuted = c.value(QStringLiteral("viewer/muted"), s.videoMuted).toBool();
    s.videoLoop = c.value(QStringLiteral("viewer/loop"), s.videoLoop).toBool();
    s.volume = qBound(0, c.value(QStringLiteral("viewer/volume"), s.volume).toInt(), 100);
    s.viewerMaxSide =
        c.value(QStringLiteral("viewer/maxSide"), s.viewerMaxSide).toInt();
    if (s.viewerMaxSide < 512)
        s.viewerMaxSide = 512;
    s.geometry = c.value(QStringLiteral("window/geometry")).toByteArray();
    s.sideSplitter = c.value(QStringLiteral("window/sideSplitter")).toByteArray();
    return s;
}

void saveSettings(const AppSettings &s) {
    QSettings c = open();
    c.setValue(QStringLiteral("cache/index"), s.indexCache);
    c.setValue(QStringLiteral("cache/thumbs"), s.thumbCache);
    c.setValue(QStringLiteral("cache/thumbSize"), s.thumbLongSide);
    c.setValue(QStringLiteral("library/root"), s.lastRoot);
    c.setValue(QStringLiteral("library/history"), s.history);
    c.setValue(QStringLiteral("general/restoreSession"), s.restoreSession);
    c.setValue(QStringLiteral("classify/enabled"), s.classifyTags);
    c.setValue(QStringLiteral("gelbooru/user"), s.gelbooruUser);
    c.setValue(QStringLiteral("gelbooru/key"), s.gelbooruKey);
    c.setValue(QStringLiteral("explicit/tags"), s.explicitTags);
    c.setValue(QStringLiteral("theme/mode"), s.themeMode);
    c.setValue(QStringLiteral("theme/accent"), s.accent);
    c.setValue(QStringLiteral("theme/stylesheet"), s.stylesheet);
    c.setValue(QStringLiteral("grid/aspectNum"), s.aspectNum);
    c.setValue(QStringLiteral("grid/aspectDen"), s.aspectDen);
    c.setValue(QStringLiteral("viewer/muted"), s.videoMuted);
    c.setValue(QStringLiteral("viewer/loop"), s.videoLoop);
    c.setValue(QStringLiteral("viewer/volume"), s.volume);
    c.setValue(QStringLiteral("viewer/maxSide"), s.viewerMaxSide);
    c.setValue(QStringLiteral("window/geometry"), s.geometry);
    c.setValue(QStringLiteral("window/sideSplitter"), s.sideSplitter);
    c.sync();
}

void clearCaches() {
    QDir(appCacheDir() + QStringLiteral("/roots")).removeRecursively();
    QDir(appCacheDir() + QStringLiteral("/thumbs")).removeRecursively();
}
