cmake_minimum_required(VERSION 3.24)

project(Envoy VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# We never install the project, and the install rules of CommonLibSSE-NG are
# written for vcpkg and break when fmt and spdlog arrive from FetchContent.
set(CMAKE_SKIP_INSTALL_RULES ON)

# ---------------------------------------------------------------- dependencies
# CommonLibSSE-NG looks for fmt and spdlog through find_package, so we slip them
# in with FetchContent and OVERRIDE_FIND_PACKAGE. vcpkg is not needed for this.
include(FetchContent)

set(FMT_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    fmt
    GIT_REPOSITORY https://github.com/fmtlib/fmt.git
    GIT_TAG 11.0.2
    GIT_SHALLOW TRUE
    OVERRIDE_FIND_PACKAGE
)

set(SPDLOG_FMT_EXTERNAL ON CACHE BOOL "" FORCE)
set(SPDLOG_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.15.0
    GIT_SHALLOW TRUE
    OVERRIDE_FIND_PACKAGE
)

FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
    GIT_SHALLOW TRUE
)

# rapidcsv is wanted by CommonLibSSE-NG itself: it reads the address library out
# of a CSV, and that is exactly the path by which VR is supported. We take only
# the header: src/ has no CMakeLists of its own, so the rapidcsv project is not
# configured.
FetchContent_Declare(
    rapidcsv
    GIT_REPOSITORY https://github.com/d99kris/rapidcsv.git
    GIT_TAG v8.99
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR src
)

FetchContent_MakeAvailable(fmt spdlog nlohmann_json rapidcsv)

set(RAPIDCSV_INCLUDE_DIRS "${rapidcsv_SOURCE_DIR}/src" CACHE PATH "" FORCE)

# We do not need the tests of the library itself - they would drag in Catch2.
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)

message(STATUS "Fetching CommonLibSSE-NG (the first time this takes a while)...")
FetchContent_Declare(
    CommonLibSSE
    GIT_REPOSITORY https://github.com/CharmedBaryon/CommonLibSSE-NG.git
    GIT_TAG v3.7.0
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(CommonLibSSE)

FetchContent_Declare(
    httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG v0.18.3
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(httplib)

# ------------------------------------------------------- the reference settings
# The single source of truth is config/envoy.default.json. It goes into the
# binary whole at build time: there is no second copy of the values in the
# code, and on the first launch the plugin unfolds this set into a file next to
# itself.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/config/envoy.default.json" ENVOY_DEFAULT_JSON)
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EnvoyDefaults.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/EnvoyDefaults.h"
    @ONLY
)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/config/envoy.default.json")

# ------------------------------------------------------------------- two layers
# The core (envoy-core) knows nothing about SKSE: neither an include nor a call.
# Everything it needs from the game it asks for through three seams -
# core/MainThread, core/GameState, core/Events - and with no seams set it
# answers itself. That is why the core builds and runs without the game, and
# why the auction can be checked with the console host out of tests/.
#
# The independence rests on this file rather than on a promise: the envoy-core
# target is not linked to CommonLibSSE by a single line, and any include of
# <SKSE/...> in the core will not build. The rule cannot be broken silently.
file(GLOB_RECURSE ENVOY_CORE_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/bus/*.cpp")
file(GLOB_RECURSE ENVOY_CORE_HEADERS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/bus/*.h")

add_library(envoy-core STATIC ${ENVOY_CORE_SOURCES} ${ENVOY_CORE_HEADERS})

target_include_directories(envoy-core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_BINARY_DIR}/generated"
)

target_link_libraries(envoy-core PUBLIC
    spdlog::spdlog
    nlohmann_json::nlohmann_json
)

if(MSVC)
    target_compile_options(envoy-core PRIVATE
        /MP /W4 /WX /permissive- /Zc:preprocessor /EHsc /utf-8
        /wd4068 /wd4189 /wd4200 /wd4201 /wd4324
    )
endif()

# ------------------------------------------------------------- the SKSE layer
# Here and only here does the game live: the Papyrus functions, the sending of
# events, the handing out of the interface to adapters, and the answers to the
# three questions of the core by the means of Skyrim.
file(GLOB_RECURSE ENVOY_SKSE_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/game/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/wire/*.cpp")
file(GLOB_RECURSE ENVOY_SKSE_HEADERS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/game/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/wire/*.h")

add_library(${PROJECT_NAME} SHARED
    ${ENVOY_SKSE_SOURCES} ${ENVOY_SKSE_HEADERS}
    "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp")

# The strict flags are for our code only. Other libraries build with their own:
# giving them /WX would mean failing on somebody else warnings.
if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE
        /MP /W4 /WX /permissive- /Zc:preprocessor /EHsc /utf-8
        /wd4068 /wd4189 /wd4200 /wd4201 /wd4324
    )
endif()

set_target_properties(${PROJECT_NAME} PROPERTIES
    OUTPUT_NAME "${PROJECT_NAME}"
    SUFFIX ".dll"
    INTERPROCEDURAL_OPTIMIZATION_RELEASE ON
)

target_compile_definitions(${PROJECT_NAME} PRIVATE
    PLUGIN_NAME="${PROJECT_NAME}"
    PLUGIN_AUTHOR="Vhodnoylogin"
    PLUGIN_VERSION="${PROJECT_VERSION}"
)

target_include_directories(${PROJECT_NAME} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/contract"
    "${CMAKE_CURRENT_BINARY_DIR}/generated"
    # REL/Relocation.h out of CommonLibSSE-NG includes rapidcsv.h, but the library
    # does not hand the path to it outside - we have to add it ourselves.
    "${RAPIDCSV_INCLUDE_DIRS}"
    # The SKSE Menu Framework header: a soft dependency, it links nothing. It sits
    # in the tree rather than being fetched, so that the bridge builds offline.
    "${CMAKE_CURRENT_SOURCE_DIR}/extern/SKSEMenuFramework"
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    envoy-core
    CommonLibSSE::CommonLibSSE
    nlohmann_json::nlohmann_json
    httplib::httplib
)

# Laying out into mods\ is done by tools/deploy.ps1 and not by a post-build
# step: writing into the build while the game is running is not allowed.

add_subdirectory(providers)

# The console host for checks: the same core, but without the game.
add_subdirectory(tests)
# The adapter is not built here: it is a module of its own with a build file of
# its own in adapter-voice. The bridge does not know about it at all - it hands
# the contract over as the SDK folder, and that is enough. Envoy still has one
# branch: the modules will move to branches of their own later, once the main
# settings are finished.
