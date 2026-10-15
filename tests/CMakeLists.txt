# Unit tests (Qt Test, headless: QCoreApplication only, temp dirs, no
# network). Run with `make test` or `ctest --test-dir build`.
find_package(Qt6 REQUIRED COMPONENTS Test)

function(add_unit_test name)
    add_executable(${name} ${ARGN})
    target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR}/src
                                               ${CMAKE_SOURCE_DIR}/src/engines)
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
    ../src/remotesource.cpp)

add_unit_test(test_explicittags test_explicittags.cpp)
add_unit_test(test_filehash test_filehash.cpp)
add_unit_test(test_metaparser test_metaparser.cpp ../src/metaparser.cpp)
add_unit_test(test_losnimport test_losnimport.cpp ../src/losnimport.cpp
              ../src/downloadmanager.cpp ../src/webdlbackend.h ${REMOTE_SOURCES}
              ../src/favdb.cpp ../src/settings.cpp ../src/metaparser.cpp
              ../src/progressbar.cpp)
add_unit_test(test_remotesource test_remotesource.cpp ${REMOTE_SOURCES})
add_unit_test(test_downloadplan test_downloadplan.cpp ../src/downloadmanager.cpp
              ../src/webdlbackend.h ${REMOTE_SOURCES} ../src/progressbar.cpp)
add_unit_test(test_settings test_settings.cpp ../src/settings.cpp
              ${REMOTE_SOURCES})
add_unit_test(test_favdb test_favdb.cpp ../src/favdb.cpp)
add_unit_test(test_cachejanitor test_cachejanitor.cpp ../src/cachejanitor.cpp
              ../src/settings.cpp ${REMOTE_SOURCES})
add_unit_test(test_tagtypes test_tagtypes.cpp ../src/tagtypes.cpp
              ${REMOTE_SOURCES})
add_unit_test(test_tagsuggest test_tagsuggest.cpp ../src/tagsuggest.cpp
              ${REMOTE_SOURCES} ../src/tagtypes.cpp)
add_unit_test(test_favicon test_favicon.cpp ../src/faviconcache.cpp
              ${REMOTE_SOURCES})
add_unit_test(test_sidecarrefresh test_sidecarrefresh.cpp ../src/sidecarrefresh.cpp
              ${REMOTE_SOURCES})
add_unit_test(test_thumbloader test_thumbloader.cpp ../src/thumbloader.cpp
              ../src/cachejanitor.cpp ${REMOTE_SOURCES} ../src/settings.cpp)
target_compile_definitions(test_thumbloader PRIVATE
    TESTFILES_DIR="${CMAKE_SOURCE_DIR}/testfiles")
add_unit_test(test_index test_index.cpp ../src/index.cpp)
add_unit_test(test_queryor test_queryor.cpp ../src/queryor.cpp)

find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    add_test(NAME check_translations
             COMMAND ${Python3_EXECUTABLE}
                     ${CMAKE_CURRENT_SOURCE_DIR}/check_translations.py
                     ${CMAKE_SOURCE_DIR}/translations)
endif()
