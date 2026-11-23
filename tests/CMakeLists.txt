# Unit tests (Qt Test, headless: QCoreApplication only, temp dirs, no
# network). Run with `ctest --test-dir build` or a single `-R` case.
find_package(Qt6 REQUIRED COMPONENTS Test)

function(add_unit_test name)
    add_executable(${name} ${ARGN})
    target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR}/src
                                               ${CMAKE_SOURCE_DIR}/src/engines
                                               ${CMAKE_SOURCE_DIR}/src/remote
                                               ${CMAKE_SOURCE_DIR}/src/library
                                               ${CMAKE_SOURCE_DIR}/src/ui)
    target_link_libraries(${name} PRIVATE Qt6::Test Qt6::Widgets Qt6::Concurrent
                                           Qt6::Sql Qt6::Multimedia
                                           Qt6::MultimediaWidgets Qt6::Network)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

# Remote-source split (remotesource + per-engine impls + quirks): every
# test that compiled remotesource.cpp gets the whole set.
set(REMOTE_SOURCES
    ../src/engines/sitequirks.cpp
    ../src/engines/booruengine.cpp
    ../src/engines/engine_danbooru.cpp
    ../src/engines/engine_gelbooru.cpp
    ../src/engines/engine_moebooru.cpp
    ../src/engines/engine_szurubooru.cpp
    ../src/engines/engine_shimmie.cpp
    ../src/engines/engine_hybooru.cpp
    ../src/engines/engine_nuuru.cpp
    ../src/engines/engine_philomena.cpp
    ../src/engines/engine_e621.cpp
    ../src/engines/engine_animepictures.cpp
    ../src/remote/remotesource.cpp
    ../src/remote/powsolver.cpp)

# Ugoira conversion + vendored miniz: every test linking
# downloadmanager.cpp gets the whole set.
set(UGOIRA_SOURCES
    ../src/library/ugoioconvert.cpp
    ../src/library/miniz/miniz.c)
include_directories(${CMAKE_SOURCE_DIR}/src/library/miniz)

# Boot migrations (src/migrations/): one file per one-time upgrade.
# Any test linking settings.cpp, favdb.cpp, cachejanitor.cpp,
# faviconcache.cpp or mainwindow*.cpp must also link these (those units
# delegate their inline upgrades to the migration helpers).
set(MIGRATION_SOURCES
    ../src/migrations/runner.cpp
    ../src/migrations/m_settings_keys.cpp
    ../src/migrations/m_booru_credentials.cpp
    ../src/migrations/m_favdb_schema.cpp
    ../src/migrations/m_favdb_hash_ids.cpp
    ../src/migrations/m_janitor_columns.cpp
    ../src/migrations/m_cache_layout.cpp
    ../src/migrations/m_favicon_adopt.cpp)

add_unit_test(test_explicittags test_explicittags.cpp)
add_unit_test(test_filehash test_filehash.cpp)
add_unit_test(test_hydrus test_hydrus.cpp ../src/remote/hydrus.cpp)
add_unit_test(test_hydrusstore test_hydrusstore.cpp
              ../src/library/hydrusstore.cpp)
add_unit_test(test_csvbackup test_csvbackup.cpp ../src/library/csvbackup.cpp)
add_unit_test(test_metaparser test_metaparser.cpp ../src/library/metaparser.cpp)
add_unit_test(test_gallerydl test_gallerydl.cpp ../src/remote/gallerydl.cpp
              ../src/library/metaparser.cpp ${UGOIRA_SOURCES})
target_compile_definitions(test_gallerydl PRIVATE
    TESTFILES_DIR="${CMAKE_SOURCE_DIR}/tests/files")
add_unit_test(test_losnimport test_losnimport.cpp ../src/library/losnimport.cpp
              ../src/remote/downloadmanager.cpp ../src/remote/downloadqueue.cpp ../src/remote/webdlbackend.h ${REMOTE_SOURCES}
              ../src/remote/postmeta.cpp
              ../src/remote/imagecache.cpp ../src/library/cachejanitor.cpp
              ${UGOIRA_SOURCES} ${MIGRATION_SOURCES}
              ../src/library/favdb.cpp ../src/settings.cpp ../src/library/metaparser.cpp
              ../src/ui/progressbar.cpp)
add_unit_test(test_remotesource test_remotesource.cpp ${REMOTE_SOURCES})
add_unit_test(test_strayresolve test_strayresolve.cpp ../src/remote/strayresolve.cpp
              ../src/library/metaparser.cpp ${REMOTE_SOURCES})
set_tests_properties(test_strayresolve PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
add_unit_test(test_downloadplan test_downloadplan.cpp ../src/remote/downloadmanager.cpp
              ../src/remote/downloadqueue.cpp ../src/remote/webdlbackend.h ${REMOTE_SOURCES} ${UGOIRA_SOURCES} ../src/ui/progressbar.cpp
              ../src/remote/postmeta.cpp ../src/library/metaparser.cpp
              ../src/remote/imagecache.cpp ../src/library/cachejanitor.cpp ../src/settings.cpp ${MIGRATION_SOURCES})
add_unit_test(test_migrations test_migrations.cpp ${MIGRATION_SOURCES}
              ../src/settings.cpp ../src/library/favdb.cpp
              ../src/library/metaparser.cpp
              ${REMOTE_SOURCES})
set_tests_properties(test_migrations PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
add_unit_test(test_settings test_settings.cpp ../src/settings.cpp
              ${REMOTE_SOURCES} ${MIGRATION_SOURCES})
add_unit_test(test_configmove test_configmove.cpp ../src/settings.cpp
              ${REMOTE_SOURCES} ${MIGRATION_SOURCES})
set_tests_properties(test_configmove PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
add_unit_test(test_favdb test_favdb.cpp ../src/library/favdb.cpp
              ../src/library/metaparser.cpp
              ../src/migrations/m_favdb_schema.cpp
              ../src/migrations/m_favdb_hash_ids.cpp)
add_unit_test(test_cachejanitor test_cachejanitor.cpp ../src/library/cachejanitor.cpp
              ../src/settings.cpp ${REMOTE_SOURCES} ${MIGRATION_SOURCES})
add_unit_test(test_cache test_cache.cpp ../src/library/cache.cpp
              ../src/settings.cpp ${REMOTE_SOURCES} ${MIGRATION_SOURCES})
set_tests_properties(test_cache PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
add_unit_test(test_cachepolicy test_cachepolicy.cpp)
add_unit_test(test_stray test_stray.cpp)
set_tests_properties(test_cachepolicy test_stray PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
add_unit_test(test_tagtypes test_tagtypes.cpp ../src/library/tagtypes.cpp
              ${REMOTE_SOURCES})
add_unit_test(test_tagcountcache test_tagcountcache.cpp
              ../src/library/tagcountcache.cpp)
add_unit_test(test_mediasize test_mediasize.cpp ../src/library/mediasize.cpp)
add_unit_test(test_tagsuggest test_tagsuggest.cpp ../src/remote/tagsuggest.cpp
              ${REMOTE_SOURCES} ../src/library/tagtypes.cpp
              ../src/library/tagcountcache.cpp
              ../src/library/index.cpp ../src/library/metaparser.cpp
              ../src/library/cache.cpp ../src/settings.cpp ${MIGRATION_SOURCES})
add_unit_test(test_favicon test_favicon.cpp ../src/remote/faviconcache.cpp
              ../src/settings.cpp
              ${REMOTE_SOURCES} ${MIGRATION_SOURCES})
add_unit_test(test_sidecarrefresh test_sidecarrefresh.cpp ../src/library/sidecarrefresh.cpp
              ${REMOTE_SOURCES})
add_unit_test(test_thumbloader test_thumbloader.cpp ../src/remote/thumbloader.cpp
              ../src/remote/webdlbackend.h
              ../src/library/cachejanitor.cpp ${REMOTE_SOURCES} ../src/settings.cpp ../src/remote/tempmedia.cpp ${MIGRATION_SOURCES})
target_compile_definitions(test_thumbloader PRIVATE
    TESTFILES_DIR="${CMAKE_SOURCE_DIR}/tests/files")
add_unit_test(test_gifview test_gifview.cpp ../src/ui/viewerpage.cpp ../src/ui/viewerpage_mediaviews.cpp ../src/ui/viewerpage_meta.cpp ../src/ui/viewerpage_tags.cpp ../src/ui/viewerpage_load.cpp ../src/ui/viewerpage_setters.cpp ../src/ui/viewerpage_detail.cpp ../src/ui/viewerpage_nav.cpp
              ../src/remote/imagecache.cpp ../src/remote/postmeta.cpp
              ../src/remote/webdlbackend.h ../src/library/cachejanitor.cpp
              ../src/ui/progressbar.cpp ../src/ui/flowlayout.cpp
              ../src/ui/metatext.cpp
              ../src/library/metaparser.cpp
              ${REMOTE_SOURCES} ../src/settings.cpp ../src/remote/tempmedia.cpp ${MIGRATION_SOURCES})
target_compile_definitions(test_gifview PRIVATE
    TESTFILES_DIR="${CMAKE_SOURCE_DIR}/tests/files")
set_tests_properties(test_gifview PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
add_unit_test(test_metathread test_metathread.cpp ../src/ui/viewerpage.cpp ../src/ui/viewerpage_mediaviews.cpp ../src/ui/viewerpage_meta.cpp ../src/ui/viewerpage_tags.cpp ../src/ui/viewerpage_load.cpp ../src/ui/viewerpage_setters.cpp ../src/ui/viewerpage_detail.cpp ../src/ui/viewerpage_nav.cpp
              ../src/remote/imagecache.cpp ../src/remote/postmeta.cpp
              ../src/remote/webdlbackend.h ../src/library/cachejanitor.cpp
              ../src/ui/progressbar.cpp ../src/ui/flowlayout.cpp
              ../src/ui/metatext.cpp
              ../src/library/metaparser.cpp
              ${REMOTE_SOURCES} ../src/settings.cpp ../src/remote/tempmedia.cpp ${MIGRATION_SOURCES})
set_tests_properties(test_metathread PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
add_unit_test(test_videopane test_videopane.cpp ../src/ui/viewerpage.cpp ../src/ui/viewerpage_mediaviews.cpp ../src/ui/viewerpage_meta.cpp ../src/ui/viewerpage_tags.cpp ../src/ui/viewerpage_load.cpp ../src/ui/viewerpage_setters.cpp ../src/ui/viewerpage_detail.cpp ../src/ui/viewerpage_nav.cpp
              ../src/remote/imagecache.cpp ../src/remote/postmeta.cpp
              ../src/remote/webdlbackend.h ../src/library/cachejanitor.cpp
              ../src/ui/progressbar.cpp ../src/ui/flowlayout.cpp
              ../src/ui/metatext.cpp
              ../src/library/metaparser.cpp
              ${REMOTE_SOURCES} ../src/settings.cpp ../src/remote/tempmedia.cpp ${MIGRATION_SOURCES})
target_compile_definitions(test_videopane PRIVATE
    TESTFILES_DIR="${CMAKE_SOURCE_DIR}/tests/files")
set_tests_properties(test_videopane PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
add_unit_test(test_index test_index.cpp ../src/library/index.cpp)
add_unit_test(test_queryor test_queryor.cpp ../src/library/queryor.cpp)
add_unit_test(test_tagcolors test_tagcolors.cpp)
add_unit_test(test_stats test_stats.cpp ../src/ui/statsdialog.cpp
              ../src/library/metaparser.cpp ${REMOTE_SOURCES})
set_tests_properties(test_stats PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
add_unit_test(test_multipick test_multipick.cpp)
set_tests_properties(test_multipick PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
add_unit_test(test_gridwindow test_gridwindow.cpp
              ../src/remote/webdlbackend.h
              ../src/ui/mainwindow.cpp ../src/ui/mainwindow_gridmenu.cpp ../src/ui/mainwindow_tabs.cpp ../src/ui/mainwindow_downloads.cpp ../src/ui/mainwindow_sidebar.cpp ../src/ui/mainwindow_wall.cpp ../src/ui/mainwindow_search.cpp ../src/ui/mainwindow_filter.cpp ../src/ui/mainwindow_stray.cpp ../src/ui/mainwindow_backup.cpp ../src/ui/mainwindow_favtab.cpp ../src/ui/mainwindow_sites.cpp ../src/ui/mainwindow_refresh.cpp ../src/ui/mainwindow_classify.cpp ../src/ui/mainwindow_scan.cpp ../src/ui/mainwindow_fill.cpp ../src/ui/mainwindow_sidebar2.cpp ../src/ui/mainwindow_thumb.cpp ../src/ui/mainwindow_suggest.cpp ../src/ui/mainwindow_heal.cpp ../src/ui/mainwindow_state.cpp ../src/ui/mainwindow_filter2.cpp ../src/ui/mainwindow_result.cpp ../src/ui/mainwindow_tagmenu.cpp ../src/ui/mainwindow_misc.cpp ../src/ui/mainwindow_relink.cpp ../src/ui/mainwindow_viewer.cpp ../src/ui/mainwindow_prefs.cpp ../src/ui/mainwindow_close.cpp ../src/ui/mainwindow_misc2.cpp ../src/ui/mainwindow_metadata.cpp ../src/ui/mainwindow_hydrus.cpp ../src/ui/metadatadialog.cpp ../src/ui/statsdialog.cpp ../src/ui/mainwindow_stats.cpp ../src/ui/mainwindow_back.cpp ../src/ui/viewerpage.cpp ../src/ui/viewerpage_mediaviews.cpp ../src/ui/viewerpage_meta.cpp ../src/ui/viewerpage_tags.cpp ../src/ui/viewerpage_load.cpp ../src/ui/viewerpage_setters.cpp ../src/ui/viewerpage_detail.cpp ../src/ui/viewerpage_nav.cpp
              ../src/ui/welcomepage.cpp ../src/ui/tabbar.cpp
              ../src/ui/theme.cpp ../src/ui/flowlayout.cpp
              ../src/ui/metatext.cpp
              ../src/ui/prefsdlg.cpp ../src/ui/prefsdlg_booru.cpp ../src/remote/cookiegrab.cpp
              ../src/ui/straydialog.cpp ../src/ui/hydrusdialog.cpp
              ../src/ui/hydrustagdialog.cpp
              ../src/library/hydrusstore.cpp
              ../src/remote/hydrus.cpp ../src/ui/progressbar.cpp
              ../src/ui/gridloadingoverlay.cpp
              ../src/remote/imagecache.cpp ../src/remote/postmeta.cpp
              ../src/remote/downloadmanager.cpp ../src/remote/downloadqueue.cpp ../src/library/losnimport.cpp
              ../src/remote/strayresolve.cpp
              ../src/remote/gallerydl.cpp
              ../src/library/favdb.cpp ../src/library/csvbackup.cpp
              ../src/library/metaparser.cpp
              ../src/library/queryor.cpp ../src/library/tagtypes.cpp
              ../src/library/scanner.cpp ../src/library/cache.cpp
              ../src/remote/thumbloader.cpp ../src/remote/tagsuggest.cpp
              ../src/library/tagcountcache.cpp ../src/library/mediasize.cpp
              ../src/remote/faviconcache.cpp ../src/library/sidecarrefresh.cpp
              ../src/library/index.cpp ../src/library/cachejanitor.cpp
              ../src/settings.cpp ${REMOTE_SOURCES} ${UGOIRA_SOURCES}
              ../src/library/settingsexport.cpp ../src/remote/tempmedia.cpp ${MIGRATION_SOURCES})
target_link_libraries(test_gridwindow PRIVATE Qt6::WebEngineWidgets Qt6::WebEngineCore)
set_tests_properties(test_gridwindow PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
add_unit_test(test_nuuru test_nuuru.cpp ${REMOTE_SOURCES}
              ../src/remote/tagsuggest.cpp ../src/library/tagtypes.cpp
              ../src/library/tagcountcache.cpp
              ../src/library/index.cpp ../src/library/metaparser.cpp
              ../src/library/cache.cpp ../src/settings.cpp ${MIGRATION_SOURCES})
add_unit_test(test_e621 test_e621.cpp ${REMOTE_SOURCES}
              ../src/remote/tagsuggest.cpp ../src/library/tagtypes.cpp
              ../src/library/tagcountcache.cpp
              ../src/library/index.cpp ../src/library/metaparser.cpp
              ../src/library/cache.cpp ../src/settings.cpp ${MIGRATION_SOURCES})
add_unit_test(test_animepictures test_animepictures.cpp ${REMOTE_SOURCES}
              ../src/remote/tagsuggest.cpp ../src/library/tagtypes.cpp
              ../src/library/tagcountcache.cpp
              ../src/library/index.cpp ../src/library/metaparser.cpp
              ../src/library/cache.cpp ../src/settings.cpp ${MIGRATION_SOURCES})
add_unit_test(test_philomena test_philomena.cpp ${REMOTE_SOURCES}
              ../src/remote/tagsuggest.cpp ../src/library/tagtypes.cpp
              ../src/library/tagcountcache.cpp
              ../src/library/index.cpp ../src/library/metaparser.cpp
              ../src/library/cache.cpp ../src/settings.cpp ${MIGRATION_SOURCES})
set_tests_properties(test_philomena PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
add_unit_test(test_commentmarks test_commentmarks.cpp ${REMOTE_SOURCES}
              ../src/remote/tagsuggest.cpp ../src/library/tagtypes.cpp
              ../src/library/tagcountcache.cpp
              ../src/library/index.cpp ../src/library/metaparser.cpp
              ../src/library/cache.cpp ../src/settings.cpp ${MIGRATION_SOURCES})
add_unit_test(test_hybooru test_hybooru.cpp ${REMOTE_SOURCES}
              ../src/remote/tagsuggest.cpp ../src/library/tagtypes.cpp
              ../src/library/tagcountcache.cpp
              ../src/library/index.cpp ../src/library/metaparser.cpp
              ../src/library/cache.cpp ../src/settings.cpp ${MIGRATION_SOURCES})
add_unit_test(test_postmeta test_postmeta.cpp ../src/remote/postmeta.cpp
              ${REMOTE_SOURCES})
add_unit_test(test_metatext test_metatext.cpp ../src/ui/metatext.cpp)
add_unit_test(test_mediafilter test_mediafilter.cpp)
add_unit_test(test_powsolver test_powsolver.cpp ../src/remote/powsolver.cpp
              ${REMOTE_SOURCES})
add_unit_test(test_settingsexport test_settingsexport.cpp
              ../src/library/settingsexport.cpp ../src/settings.cpp ../src/library/favdb.cpp
              ../src/library/metaparser.cpp
              ${REMOTE_SOURCES} ${MIGRATION_SOURCES})
add_unit_test(test_ugoira test_ugoira.cpp ../src/library/ugoioconvert.cpp
              ../src/library/miniz/miniz.c)
target_compile_definitions(test_ugoira PRIVATE
    TESTFILES_DIR="${CMAKE_SOURCE_DIR}/tests/files")
target_include_directories(test_ugoira PRIVATE
    ${CMAKE_SOURCE_DIR}/src/library/miniz)
add_unit_test(test_tabbar test_tabbar.cpp ../src/ui/tabbar.cpp)
set_tests_properties(test_tabbar PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
add_unit_test(test_welcomepage test_welcomepage.cpp ../src/ui/welcomepage.cpp
              ../src/icons.qrc)
target_compile_definitions(test_welcomepage PRIVATE
    TESTFILES_DIR="${CMAKE_SOURCE_DIR}/tests/files")
set_tests_properties(test_welcomepage PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
add_unit_test(test_theme test_theme.cpp ../src/ui/theme.cpp)
if(APP_THEME_FILES)
    qt_add_resources(test_theme "test_theme_resources"
        PREFIX "/themes"
        BASE "${CMAKE_SOURCE_DIR}/assets/themes"
        FILES ${APP_THEME_FILES})
endif()
set_tests_properties(test_theme PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
add_unit_test(test_scanner test_scanner.cpp ../src/library/scanner.cpp
              ../src/library/metaparser.cpp ../src/library/cache.cpp
              ../src/settings.cpp ${REMOTE_SOURCES} ${MIGRATION_SOURCES})
add_unit_test(test_security test_security.cpp ${REMOTE_SOURCES}
              ../src/library/metaparser.cpp ../src/library/losnimport.cpp
              ../src/library/favdb.cpp ../src/settings.cpp
              ../src/library/queryor.cpp
              ../src/remote/downloadmanager.cpp ../src/remote/postmeta.cpp
              ../src/remote/downloadqueue.cpp
              ../src/remote/webdlbackend.h
              ../src/remote/imagecache.cpp ../src/library/cachejanitor.cpp
              ${UGOIRA_SOURCES}
              ../src/ui/progressbar.cpp ${MIGRATION_SOURCES})

find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    add_test(NAME check_translations
             COMMAND ${Python3_EXECUTABLE}
                     ${CMAKE_CURRENT_SOURCE_DIR}/check_translations.py
                     ${CMAKE_SOURCE_DIR}/translations)
endif()
