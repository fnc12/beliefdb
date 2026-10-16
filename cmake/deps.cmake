# Dependencies via FetchContent, with a local-checkout override.
#
# Problem: FetchContent cloning nlohmann/json from GitHub takes 10-20 minutes
# (the repository carries years of docs and test data), and every clean build
# repeats it. Solution: keep local checkouts under one directory (default
# ~/deps, same layout on every machine) and point FetchContent at them with
# FETCHCONTENT_SOURCE_DIR_<NAME>. If a checkout is missing, FetchContent falls
# back to the canonical URL below, so the build never depends on the cache.
#
# Expected layout of ${BELIEFDB_DEPS_DIR}:
#   json/        nlohmann/json release tarball, e.g. v3.12.0 json.tar.xz
#   sqlite_orm/  git clone of https://github.com/fnc12/sqlite_orm (branch dev)
#   doctest/     git clone of https://github.com/doctest/doctest (tag v2.4.12)

set(BELIEFDB_DEPS_DIR "$ENV{HOME}/deps" CACHE PATH
    "Directory with local checkouts of dependencies (json/, sqlite_orm/, doctest/)")

include(FetchContent)

# bdb_declare(<name> <subdir-in-deps> <FetchContent args for the canonical origin...>)
function(bdb_declare name subdir)
    string(TOUPPER "${name}" upper)
    if(EXISTS "${BELIEFDB_DEPS_DIR}/${subdir}/CMakeLists.txt")
        set(FETCHCONTENT_SOURCE_DIR_${upper} "${BELIEFDB_DEPS_DIR}/${subdir}" CACHE PATH "" FORCE)
        message(STATUS "beliefdb: ${name} from local checkout ${BELIEFDB_DEPS_DIR}/${subdir}")
    else()
        message(STATUS "beliefdb: ${name} not found in ${BELIEFDB_DEPS_DIR}, fetching from origin")
    endif()
    FetchContent_Declare(${name} ${ARGN})
endfunction()

# Canonical origins (used only when the local checkout is absent).
# json: the release tarball is ~110 KB; the git repository is the slow path,
#       which is why it is deliberately NOT referenced here.
#       git: https://github.com/nlohmann/json.git (tag v3.12.0)
bdb_declare(json json
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
    URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
bdb_declare(sqlite_orm sqlite_orm
    GIT_REPOSITORY https://github.com/fnc12/sqlite_orm.git
    GIT_TAG dev
    GIT_SHALLOW TRUE)
bdb_declare(doctest doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG v2.4.12
    GIT_SHALLOW TRUE)

set(JSON_BuildTests OFF CACHE INTERNAL "")
set(JSON_Install OFF CACHE INTERNAL "")
set(SQLITE_ORM_ENABLE_CXX_20 ON CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)        # sqlite_orm's own tests
set(DOCTEST_NO_INSTALL ON CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(json sqlite_orm doctest)

# Note: dependency include dirs are deliberately plain -I, not SYSTEM. With
# -isystem, Apple clang searched /usr/local/include first and a stale global
# sqlite_orm install shadowed the checkout in ~/deps. Dependency warnings are
# silenced where the headers are included (src/schema.hpp) instead.
