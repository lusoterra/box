# Unit tests (Qt Test, headless: QCoreApplication only, temp dirs, no
# network). Run with `make test` or `ctest --test-dir build`.
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
    ../src/remote/remotesource.cpp)

add_unit_test(test_explicittags test_explicittags.cpp)
add_unit_test(test_filehash test_filehash.cpp)
add_unit_test(test_metaparser test_metaparser.cpp ../src/library/metaparser.cpp)
add_unit_test(test_losnimport test_losnimport.cpp ../src/library/losnimport.cpp
              ../src/remote/downloadmanager.cpp ../src/remote/webdlbackend.h ${REMOTE_SOURCES}
              ../src/library/favdb.cpp ../src/settings.cpp ../src/library/metaparser.cpp
              ../src/ui/progressbar.cpp)
add_unit_test(test_remotesource test_remotesource.cpp ${REMOTE_SOURCES})
add_unit_test(test_downloadplan test_downloadplan.cpp ../src/remote/downloadmanager.cpp
              ../src/remote/webdlbackend.h ${REMOTE_SOURCES} ../src/ui/progressbar.cpp)
add_unit_test(test_settings test_settings.cpp ../src/settings.cpp
              ${REMOTE_SOURCES})
add_unit_test(test_favdb test_favdb.cpp ../src/library/favdb.cpp)
add_unit_test(test_cachejanitor test_cachejanitor.cpp ../src/library/cachejanitor.cpp
              ../src/settings.cpp ${REMOTE_SOURCES})
add_unit_test(test_tagtypes test_tagtypes.cpp ../src/library/tagtypes.cpp
              ${REMOTE_SOURCES})
add_unit_test(test_tagsuggest test_tagsuggest.cpp ../src/remote/tagsuggest.cpp
              ${REMOTE_SOURCES} ../src/library/tagtypes.cpp
              ../src/library/index.cpp ../src/library/metaparser.cpp
              ../src/library/cache.cpp ../src/settings.cpp)
add_unit_test(test_favicon test_favicon.cpp ../src/remote/faviconcache.cpp
              ${REMOTE_SOURCES})
add_unit_test(test_sidecarrefresh test_sidecarrefresh.cpp ../src/library/sidecarrefresh.cpp
              ${REMOTE_SOURCES})
add_unit_test(test_thumbloader test_thumbloader.cpp ../src/remote/thumbloader.cpp
              ../src/library/cachejanitor.cpp ${REMOTE_SOURCES} ../src/settings.cpp)
target_compile_definitions(test_thumbloader PRIVATE
    TESTFILES_DIR="${CMAKE_SOURCE_DIR}/tests/files")
add_unit_test(test_gifview test_gifview.cpp ../src/ui/viewerpage.cpp
              ../src/remote/imagecache.cpp ../src/remote/postmeta.cpp
              ../src/remote/webdlbackend.h ../src/library/cachejanitor.cpp
              ../src/ui/progressbar.cpp ../src/ui/flowlayout.cpp
              ${REMOTE_SOURCES} ../src/settings.cpp)
target_compile_definitions(test_gifview PRIVATE
    TESTFILES_DIR="${CMAKE_SOURCE_DIR}/tests/files")
set_tests_properties(test_gifview PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
add_unit_test(test_videopane test_videopane.cpp ../src/ui/viewerpage.cpp
              ../src/remote/imagecache.cpp ../src/remote/postmeta.cpp
              ../src/remote/webdlbackend.h ../src/library/cachejanitor.cpp
              ../src/ui/progressbar.cpp ../src/ui/flowlayout.cpp
              ${REMOTE_SOURCES} ../src/settings.cpp)
target_compile_definitions(test_videopane PRIVATE
    TESTFILES_DIR="${CMAKE_SOURCE_DIR}/tests/files")
set_tests_properties(test_videopane PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
add_unit_test(test_index test_index.cpp ../src/library/index.cpp)
add_unit_test(test_queryor test_queryor.cpp ../src/library/queryor.cpp)
add_unit_test(test_scanner test_scanner.cpp ../src/library/scanner.cpp
              ../src/library/metaparser.cpp ../src/library/cache.cpp
              ../src/settings.cpp ${REMOTE_SOURCES})
add_unit_test(test_security test_security.cpp ${REMOTE_SOURCES}
              ../src/library/metaparser.cpp ../src/library/losnimport.cpp
              ../src/library/favdb.cpp ../src/settings.cpp
              ../src/library/queryor.cpp
              ../src/remote/downloadmanager.cpp
              ../src/remote/webdlbackend.h
              ../src/ui/progressbar.cpp)

find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    add_test(NAME check_translations
             COMMAND ${Python3_EXECUTABLE}
                     ${CMAKE_CURRENT_SOURCE_DIR}/check_translations.py
                     ${CMAKE_SOURCE_DIR}/translations)
endif()
