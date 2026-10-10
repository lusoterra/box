#pragma once

// Persistent options (~/.config/booru-explorer/settings.ini).
// Single source of truth: defaults live in AppSettings, the preferences
// dialog edits a copy, MainWindow applies + saves.
#include <QByteArray>
#include <QColor>
#include <QString>
#include <QStringList>

#include "explicittags.h"

struct AppSettings {
    // Library
    bool indexCache = true;
    bool thumbCache = true;
    int thumbLongSide = 360; // 256/360/512/768
    QString lastRoot;
    QStringList history; // most-recent-first archive roots, cap 10
    bool restoreSession = false; // skip welcome, always reload last session
    // Online tag-category lookup for untyped tags (cached forever).
    bool classifyTags = false;
    // Optional Gelbooru credentials (user ID + API key, both required).
    // Stored plaintext in settings.ini like most API clients do.
    QString gelbooruUser;
    QString gelbooruKey;
    // Explicit-detection tags (one per line in Preferences). Defaults to the
    // built-in set; saved once the dialog runs.
    QStringList explicitTags = defaultExplicitTagList();
    // Appearance (KeePassXC-style: Fusion pinned + own palette by default)
    QString themeMode = QStringLiteral("builtin"); // "builtin" | "native"
    QString accent = QStringLiteral("#3a7ec2");
    QString stylesheet; // optional custom .qss on top, empty = none
    // Grid
    int aspectNum = 1;
    int aspectDen = 1;
    // Playback
    bool videoMuted = true;
    bool videoLoop = true;
    int volume = 100; // 0..100 default output volume
    // Viewer
    int viewerMaxSide = 2560; // huge-image decode cap
    // Window
    QByteArray geometry;
    QByteArray sideSplitter; // sidebar section sizes
};

QString settingsFilePath();
QString appCacheDir(); // QStandardPaths::CacheLocation (includes org/app names)
AppSettings loadSettings();
void saveSettings(const AppSettings &s);

// Wipes the scan-index + thumbnail caches (fresh rescan/re-decode after).
// Only our own cache subdirs are touched.
void clearCaches();
