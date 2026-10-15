cmake_minimum_required(VERSION 3.16)
project(BooruExplorer VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Concurrent Sql Multimedia MultimediaWidgets Network LinguistTools Test WebEngineWidgets WebEngineCore)

add_executable(booru-explorer
    src/main.cpp
    src/metaparser.cpp
    src/scanner.cpp
    src/cache.cpp
    src/cachejanitor.cpp
    src/favdb.cpp
    src/index.cpp
    src/settings.cpp
    src/tagtypes.cpp
    src/postmeta.cpp
    src/engines/boorutypes.h
    src/engines/sitequirks.h
    src/engines/sitequirks.cpp
    src/engines/booruengine.h
    src/engines/booruengine.cpp
    src/engines/engine_danbooru.cpp
    src/engines/engine_gelbooru.cpp
    src/engines/engine_moebooru.cpp
    src/engines/engine_szurubooru.cpp
    src/engines/engine_shimmie.cpp
    src/remotesource.cpp
    src/downloadmanager.cpp
    src/imagecache.cpp
    src/thumbloader.cpp
    src/theme.cpp
    src/flowlayout.cpp
    src/tabbar.cpp
    src/tagsuggest.cpp
    src/faviconcache.cpp
    src/sidecarrefresh.cpp
    src/webdlbackend.h
    src/losnimport.cpp
    src/queryor.cpp
    src/cookiegrab.cpp
    src/progressbar.cpp
    src/viewerpage.cpp
    src/welcomepage.cpp
    src/prefsdlg.cpp
    src/mainwindow.cpp
    src/icons.qrc
)

target_include_directories(booru-explorer PRIVATE src src/engines)
target_link_libraries(booru-explorer PRIVATE Qt6::Widgets Qt6::Concurrent Qt6::Sql
                                             Qt6::Multimedia Qt6::MultimediaWidgets
                                             Qt6::Network Qt6::WebEngineWidgets Qt6::WebEngineCore)
target_compile_definitions(booru-explorer PRIVATE
    BOORU_EXPLORER_VERSION="${PROJECT_VERSION}")

# i18n: UI strings are tr()-wrapped (English source for now).
#   cmake --build build --target update_translations  # refresh .ts from sources
# Translators copy translations/booru-explorer.ts to
# translations/booru-explorer_<locale>.ts (e.g. _de, _ja) and fill in the
# translations; the glob below picks new files up on reconfigure, and the
# matching :/i18n/booru-explorer_<locale>.qm embeds + loads automatically.
file(GLOB APP_TS_FILES CONFIGURE_DEPENDS
     ${CMAKE_CURRENT_SOURCE_DIR}/translations/booru-explorer*.ts)
if(NOT APP_TS_FILES)
    file(TOUCH ${CMAKE_CURRENT_SOURCE_DIR}/translations/booru-explorer.ts)
    set(APP_TS_FILES ${CMAKE_CURRENT_SOURCE_DIR}/translations/booru-explorer.ts)
endif()
qt_add_translations(booru-explorer
    TS_FILES ${APP_TS_FILES}
    RESOURCE_PREFIX "/i18n"
)

include(GNUInstallDirs)
include(CTest)
add_subdirectory(tests)
install(TARGETS booru-explorer RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES packaging/booru-explorer.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES icon.png
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/512x512/apps
        RENAME booru-explorer.png)
