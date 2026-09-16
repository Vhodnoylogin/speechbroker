cmake_minimum_required(VERSION 3.24)

# The Speech Broker test adapter: an ordinary SKSE plugin, which is to say a mod. It
# talks to the bridge by calling a function inside the process of the game;
# outward, to the voice service, it goes over HTTP by itself. It is built apart
# from the bridge and knows nothing about its sources - only the contract is
# taken from the bridge, and it is taken from the installed mod, the SDK folder.
# The path to it is set in config/build.json under the sdk key.
project(SpeechBrokerVoiceAdapter VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_SKIP_INSTALL_RULES ON)

# ---------------------------------------------------------------- the contract
# Where the SDK of the bridge lies is a setting, not a constant: we read it out
# of the same file the lay-out scripts use.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/config/build.json" SPEECHBROKER_BUILD_JSON)
string(JSON SPEECHBROKER_SDK GET "${SPEECHBROKER_BUILD_JSON}" deploy sdk)
if(NOT EXISTS "${SPEECHBROKER_SDK}/speechbroker-adapter.h")
    message(FATAL_ERROR
        "The contract of the bridge was not found: ${SPEECHBROKER_SDK}/speechbroker-adapter.h\n"
        "Lay the bridge out first - it publishes the SDK folder inside its own mod.")
endif()
message(STATUS "Speech Broker contract: ${SPEECHBROKER_SDK}")

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
# the header.
FetchContent_Declare(
    rapidcsv
    GIT_REPOSITORY https://github.com/d99kris/rapidcsv.git
    GIT_TAG v8.99
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR src
)

FetchContent_MakeAvailable(fmt spdlog nlohmann_json rapidcsv)
set(RAPIDCSV_INCLUDE_DIRS "${rapidcsv_SOURCE_DIR}/src" CACHE PATH "" FORCE)

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

# ---------------------------------------------------------------- our plugin
file(GLOB_RECURSE ADAPTER_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
add_library(SpeechBrokerVoiceAdapter SHARED ${ADAPTER_SOURCES})

if(MSVC)
    target_compile_options(SpeechBrokerVoiceAdapter PRIVATE
        /MP /W4 /WX /permissive- /Zc:preprocessor /EHsc /utf-8
        /wd4068 /wd4189 /wd4200 /wd4201 /wd4324
    )
endif()

set_target_properties(SpeechBrokerVoiceAdapter PROPERTIES OUTPUT_NAME "SpeechBrokerVoiceAdapter" SUFFIX ".dll")

target_compile_definitions(SpeechBrokerVoiceAdapter PRIVATE
    PLUGIN_NAME="SpeechBrokerVoiceAdapter"
    PLUGIN_AUTHOR="Vhodnoylogin"
    PLUGIN_VERSION="${PROJECT_VERSION}"
)

target_include_directories(SpeechBrokerVoiceAdapter PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${SPEECHBROKER_SDK}"
    "${RAPIDCSV_INCLUDE_DIRS}"
)

target_link_libraries(SpeechBrokerVoiceAdapter PRIVATE
    CommonLibSSE::CommonLibSSE
    nlohmann_json::nlohmann_json
    httplib::httplib
)
