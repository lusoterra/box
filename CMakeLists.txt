cmake_minimum_required(VERSION 3.16)
project(Box VERSION 0.14.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Concurrent Sql Multimedia MultimediaWidgets Network LinguistTools Test WebEngineWidgets WebEngineCore)

add_executable(box
    src/main.cpp
    src/applog.cpp
    src/library/metaparser.cpp
    src/library/scanner.cpp
    src/library/cache.cpp
    src/library/cachejanitor.cpp
    src/library/favdb.cpp
    src/library/hydrusstore.cpp
    src/library/index.cpp
    src/settings.cpp
    src/library/tagtypes.cpp
    src/library/tagcountcache.cpp
    src/library/mediasize.cpp
    src/remote/postmeta.cpp
    src/remote/tempmedia.cpp
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
    src/engines/engine_hybooru.cpp
    src/engines/engine_nuuru.cpp
    src/engines/engine_philomena.cpp
    src/engines/engine_e621.cpp
    src/engines/engine_animepictures.cpp
    src/engines/engine_shuushuu.cpp
    src/engines/enginedetect.cpp
    src/library/ugoioconvert.cpp
    src/library/miniz/miniz.c
    src/remote/remotesource.cpp
    src/remote/strayresolve.cpp
    src/remote/hydrus.cpp
    src/remote/gallerydl.cpp
    src/remote/shareserver.cpp
    src/remote/powsolver.cpp
    src/remote/downloadmanager.cpp
    src/remote/downloadqueue.cpp
    src/remote/imagecache.cpp
    src/remote/thumbloader.cpp
    src/ui/theme.cpp
    src/ui/flowlayout.cpp
    src/ui/tabbar.cpp
    src/remote/tagsuggest.cpp
    src/remote/faviconcache.cpp
    src/library/sidecarrefresh.cpp
    src/library/settingsexport.cpp
    src/library/phash.cpp
    src/library/dedupdb.cpp
    src/library/dedupgroups.cpp
    src/library/dedupscanner.cpp
    src/remote/webdlbackend.h
    src/remote/netpolicy.h
    src/library/losnimport.cpp
    src/library/csvbackup.cpp
    src/library/queryor.cpp
    src/library/sharecatalog.cpp
    src/migrations/runner.cpp
    src/migrations/m_settings_keys.cpp
    src/migrations/m_booru_credentials.cpp
    src/migrations/m_favdb_schema.cpp
    src/migrations/m_favdb_hash_ids.cpp
    src/migrations/m_janitor_columns.cpp
    src/migrations/m_cache_layout.cpp
    src/migrations/m_favicon_adopt.cpp
    src/remote/cookiegrab.cpp
    src/ui/progressbar.cpp
    src/ui/gridloadingoverlay.cpp
    src/ui/metatext.cpp
    src/ui/viewerpage.cpp
    src/ui/viewerpage_mediaviews.cpp
    src/ui/viewerpage_meta.cpp
    src/ui/viewerpage_tags.cpp
    src/ui/viewerpage_load.cpp
    src/ui/viewerpage_setters.cpp
    src/ui/viewerpage_detail.cpp
    src/ui/viewerpage_nav.cpp
    src/ui/welcomepage.cpp
    src/ui/prefsdlg.cpp
    src/ui/prefsdlg_booru.cpp
    src/ui/straydialog.cpp
    src/ui/hydrusdialog.cpp
    src/ui/hydrustagdialog.cpp
    src/ui/sharedialog.cpp
    src/ui/mainwindow.cpp
    src/ui/mainwindow_gridmenu.cpp
    src/ui/mainwindow_tabs.cpp
    src/ui/mainwindow_downloads.cpp
    src/ui/mainwindow_sidebar.cpp
    src/ui/mainwindow_wall.cpp
    src/ui/mainwindow_search.cpp
    src/ui/mainwindow_filter.cpp
    src/ui/mainwindow_stray.cpp
    src/ui/mainwindow_backup.cpp
    src/ui/mainwindow_favtab.cpp
    src/ui/mainwindow_sites.cpp
    src/ui/mainwindow_refresh.cpp
    src/ui/mainwindow_classify.cpp
    src/ui/mainwindow_scan.cpp
    src/ui/mainwindow_fill.cpp
    src/ui/mainwindow_thumb.cpp
    src/ui/mainwindow_suggest.cpp
    src/ui/mainwindow_heal.cpp
    src/ui/mainwindow_state.cpp
    src/ui/mainwindow_filter2.cpp
    src/ui/mainwindow_result.cpp
    src/ui/mainwindow_tagmenu.cpp
    src/ui/mainwindow_misc.cpp
    src/ui/mainwindow_relink.cpp
    src/ui/mainwindow_prefs.cpp
    src/ui/mainwindow_close.cpp
    src/ui/mainwindow_misc2.cpp
    src/ui/mainwindow_hydrus.cpp
    src/ui/mainwindow_share.cpp
    src/ui/mainwindow_dedup.cpp
    src/ui/dedupdialog.cpp
    src/ui/mainwindow_metadata.cpp
    src/ui/metadatadialog.cpp
    src/ui/statsdialog.cpp
    src/ui/mainwindow_stats.cpp
    src/ui/mainwindow_back.cpp
    src/ui/mainwindow_viewer.cpp
    src/ui/mainwindow_sidebar2.cpp
    src/icons.qrc
    src/seed.qrc
)

target_include_directories(box PRIVATE src src/engines src/remote src/library src/library/miniz src/ui)
target_link_libraries(box PRIVATE Qt6::Widgets Qt6::Concurrent Qt6::Sql
                                             Qt6::Multimedia Qt6::MultimediaWidgets
                                             Qt6::Network Qt6::WebEngineWidgets Qt6::WebEngineCore)
# Export function names so box-crash.log backtraces are readable even in
# the optimised package build (addresses alone need the unstripped binary).
if(UNIX AND NOT APPLE)
    target_link_options(box PRIVATE "-rdynamic")
endif()
target_compile_definitions(box PRIVATE
    BOX_VERSION="${PROJECT_VERSION}")

# i18n: UI strings are tr()-wrapped (English source for now).
#   cmake --build build --target update_translations  # refresh .ts from sources
# Translators copy translations/box.ts to
# translations/box_<locale>.ts (e.g. _de, _ja) and fill in the
# translations; the glob below picks new files up on reconfigure, and the
# matching :/i18n/box_<locale>.qm embeds + loads automatically.
file(GLOB APP_TS_FILES CONFIGURE_DEPENDS
     ${CMAKE_CURRENT_SOURCE_DIR}/translations/box*.ts)
if(NOT APP_TS_FILES)
    file(TOUCH ${CMAKE_CURRENT_SOURCE_DIR}/translations/box.ts)
    set(APP_TS_FILES ${CMAKE_CURRENT_SOURCE_DIR}/translations/box.ts)
endif()
qt_add_translations(box
    TS_FILES ${APP_TS_FILES}
    QM_FILES_OUTPUT_VARIABLE APP_QM_FILES
)
# Explicit embed: the .qm files must live in the binary (verified by
# strings: without this, :/i18n/ is empty at runtime and every locale
# silently falls back to English).
foreach(qm IN LISTS APP_QM_FILES)
    get_filename_component(qm_name "${qm}" NAME)
    set_source_files_properties("${qm}" PROPERTIES QT_RESOURCE_ALIAS "${qm_name}")
endforeach()
qt_add_resources(box "app_translations"
    PREFIX "/i18n"
    FILES ${APP_QM_FILES}
)

# Themes: drop a new `assets/themes/<key>.ini` in place and reconfigure; it
# is embedded as :/themes/<key>.ini and appears in Preferences. User copies
# under ~/.config/box/themes/ are discovered at runtime without a rebuild.
file(GLOB APP_THEME_FILES CONFIGURE_DEPENDS
     ${CMAKE_CURRENT_SOURCE_DIR}/assets/themes/*.ini)
if(APP_THEME_FILES)
    foreach(theme IN LISTS APP_THEME_FILES)
        get_filename_component(theme_name "${theme}" NAME)
        set_source_files_properties("${theme}" PROPERTIES
            QT_RESOURCE_ALIAS "${theme_name}")
    endforeach()
    qt_add_resources(box "app_themes"
        PREFIX "/themes"
        FILES ${APP_THEME_FILES}
    )
endif()

# Box Share web UI: a single self-contained index.html (Svelte/Vite
# singlefile) served at the LAN share root. The built file is committed so
# packagers do not need Node at build time; regenerate with
# `npm ci && npm run build` in web/ after frontend changes.
set(BOX_SHARE_WEB "${CMAKE_CURRENT_SOURCE_DIR}/web/dist/index.html")
if(EXISTS "${BOX_SHARE_WEB}")
    set_source_files_properties("${BOX_SHARE_WEB}" PROPERTIES
        QT_RESOURCE_ALIAS "index.html")
    qt_add_resources(box "share_web"
        PREFIX "/share"
        FILES "${BOX_SHARE_WEB}")
else()
    message(STATUS "Box Share: web/dist/index.html missing; serving the plain fallback page")
endif()

include(GNUInstallDirs)
include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
install(TARGETS box RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES assets/box.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES assets/booru-explorer.svg
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
install(FILES docs/box.1
        DESTINATION ${CMAKE_INSTALL_MANDIR}/man1)
install(FILES assets/completions/box.bash
        DESTINATION ${CMAKE_INSTALL_DATADIR}/bash-completion/completions
        RENAME box)
install(FILES assets/completions/_box
        DESTINATION ${CMAKE_INSTALL_DATADIR}/zsh/site-functions)
install(FILES assets/completions/box.fish
        DESTINATION ${CMAKE_INSTALL_DATADIR}/fish/vendor_completions.d)
