cmake_minimum_required(VERSION 3.16)
project(BooruExplorer VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Concurrent Sql Multimedia MultimediaWidgets Network)

add_executable(booru-explorer
    src/main.cpp
    src/metaparser.cpp
    src/scanner.cpp
    src/cache.cpp
    src/index.cpp
    src/settings.cpp
    src/tagtypes.cpp
    src/thumbloader.cpp
    src/theme.cpp
    src/flowlayout.cpp
    src/viewerpage.cpp
    src/welcomepage.cpp
    src/prefsdlg.cpp
    src/mainwindow.cpp
    src/icons.qrc
)

target_include_directories(booru-explorer PRIVATE src)
target_link_libraries(booru-explorer PRIVATE Qt6::Widgets Qt6::Concurrent Qt6::Sql
                                             Qt6::Multimedia Qt6::MultimediaWidgets
                                             Qt6::Network)

include(GNUInstallDirs)
install(TARGETS booru-explorer RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES packaging/booru-explorer.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES icon.png
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/512x512/apps
        RENAME booru-explorer.png)
