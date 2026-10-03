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

# ---------------------------------------------------------------- our plugin
# The sources are gathered rather than listed, and CONFIGURE_DEPENDS is what makes
# that honest: a new file under src/ - the ears arrived as fourteen of them, in
# src/audio and src/turn - is picked up by the next build without anybody having to
# remember this line. The headers are named for the same reason the sources are:
# they compile nothing, but a header that is in the target shows up in the solution
# and is searched by the IDE, and a header nobody can find is a header nobody reads.
file(GLOB_RECURSE ADAPTER_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
file(GLOB_RECURSE ADAPTER_HEADERS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h")
add_library(SpeechBrokerVoiceAdapter SHARED ${ADAPTER_SOURCES} ${ADAPTER_HEADERS})

# So that the folders of the ears do not arrive in the solution as one flat heap.
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/src" FILES ${ADAPTER_SOURCES} ${ADAPTER_HEADERS})

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

    # The ears: the microphone belongs to the adapter, so the adapter talks to
    # WASAPI itself.
    #
    # ole32 is already in the default libraries CMake passes to MSVC; it is named
    # here anyway, because from now on this target depends on COM deliberately
    # rather than by inheritance, and a default that changes must not take the
    # capture with it. avrt is the multimedia class scheduler
    # (AvSetMmThreadCharacteristics): the capture pump asks to be scheduled as
    # audio, which is what keeps it from being starved by a game that is using
    # every core it can reach.
    ole32
    avrt
)
