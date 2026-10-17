cmake_minimum_required(VERSION 3.24)
project(beliefdb VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(BELIEFDB_BUILD_TESTS "Build unit tests" ON)
option(BELIEFDB_BUILD_CLI "Build the belief command-line tool" ON)
option(BELIEFDB_SANITIZERS "Build with ASan/UBSan" OFF)

include(cmake/deps.cmake)
find_package(SQLite3 REQUIRED)

add_library(beliefdb_warnings INTERFACE)
target_compile_options(beliefdb_warnings INTERFACE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion>
    # gcc warns on every designated initializer that omits defaulted members; that is the point of them
    $<$<CXX_COMPILER_ID:GNU>:-Wno-missing-field-initializers>)
if(BELIEFDB_SANITIZERS)
    target_compile_options(beliefdb_warnings INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(beliefdb_warnings INTERFACE -fsanitize=address,undefined)
endif()

# ---- library -------------------------------------------------------------
add_library(beliefdb
    src/store.cpp
    src/types.cpp
    src/scope.cpp
    src/value.cpp
    src/util.cpp
    src/sha256.cpp
    src/embed.cpp)
add_library(beliefdb::beliefdb ALIAS beliefdb)
target_include_directories(beliefdb PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(beliefdb
    PUBLIC nlohmann_json::nlohmann_json
    PRIVATE sqlite_orm::sqlite_orm SQLite::SQLite3 httplib::httplib beliefdb_warnings)
target_compile_features(beliefdb PUBLIC cxx_std_20)
# sqlite_orm enables its JSON functions when SQLite >= 3.38 or SQLITE_ENABLE_JSON1 is
# defined. Older distributions (Ubuntu 22.04 ships 3.37) build SQLite with JSON1 but
# the header does not say so; `pragma compile_options` on such a box lists ENABLE_JSON1.
if(SQLite3_VERSION VERSION_LESS 3.38)
    message(STATUS "beliefdb: SQLite ${SQLite3_VERSION} < 3.38, assuming the JSON1 extension is compiled in")
    target_compile_definitions(beliefdb PRIVATE SQLITE_ENABLE_JSON1)
endif()

# ---- cli -----------------------------------------------------------------
if(BELIEFDB_BUILD_CLI)
    add_executable(belief cli/belief.cpp)
    target_link_libraries(belief PRIVATE beliefdb::beliefdb beliefdb_warnings)
endif()

# ---- daemon --------------------------------------------------------------
add_library(beliefd_core STATIC daemon/server.cpp)
target_include_directories(beliefd_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/daemon)
target_link_libraries(beliefd_core PUBLIC beliefdb::beliefdb httplib::httplib PRIVATE beliefdb_warnings)
add_executable(beliefd daemon/beliefd.cpp)
target_link_libraries(beliefd PRIVATE beliefd_core beliefdb_warnings)

# ---- tests ---------------------------------------------------------------
if(BELIEFDB_BUILD_TESTS)
    enable_testing()
    add_executable(beliefdb_tests
        tests/main.cpp
        tests/test_invariants.cpp
        tests/test_why.cpp
        tests/test_scope_value.cpp
        tests/test_nutrition.cpp
        tests/test_changes.cpp
        tests/test_conflicts.cpp
        tests/test_discipline.cpp
        tests/test_search.cpp
        tests/test_server.cpp)
    target_link_libraries(beliefdb_tests PRIVATE beliefd_core doctest::doctest httplib::httplib beliefdb_warnings)  # beliefd_core carries beliefdb
    add_test(NAME beliefdb_tests COMMAND beliefdb_tests)
endif()
