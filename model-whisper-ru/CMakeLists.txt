cmake_minimum_required(VERSION 3.24)

# The shim of a model mod: an ordinary SKSE plugin, which is to say a mod. It
# talks to the voice adapter by calling a function inside the process of the
# game; outward, to the model, it raises a child process and talks to it down a
# pipe. There is not a line of the adapter's sources here and there must not be -
# only the contract, and the contract is taken from the INSTALLED mod, exactly as
# any third party's model mod would take it.
#
# The child is a project of its own in child/ with its own CMakeLists.txt, and it
# is NOT pulled in here. That is on purpose: the two have nothing in common but
# one file - src/core/Wire.cpp, the protocol - the child must build on a machine
# with no CommonLibSSE anywhere near it, and a failure in one of them has to be
# readable without the other's output in front of it.
project(SpeechBrokerModelWhisperRu VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_SKIP_INSTALL_RULES ON)

# ---------------------------------------------------------------- the contract
# Where the SDK of the adapter lies is a setting, not a constant: we read it out
# of the same file the lay-out scripts use.
#
# THE INSTALLED SDK IS THE REAL PATH, and a model mod written by somebody else
# has no other. The fallback below is for developing the adapter and this mod in
# one tree at one time, where the laid-out copy is by definition older than the
# header being edited; it says so in the configure output rather than passing
# silently, because a build that quietly used the wrong contract is the defect
# this whole arrangement exists to prevent.
#
# The key is read with ERROR_VARIABLE rather than bare: string(JSON ... GET) on a
# missing member is a hard configure error, and a build.json edited by hand is
# exactly where a missing member comes from. A missing key is the same situation
# as an SDK that has not been laid out, and it deserves the same sentence.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/config/build.json" WHISPERRU_BUILD_JSON)
string(JSON WHISPERRU_SDK ERROR_VARIABLE WHISPERRU_SDK_ERROR
       GET "${WHISPERRU_BUILD_JSON}" deploy sdk)
if(WHISPERRU_SDK_ERROR)
    set(WHISPERRU_SDK "config/build.json has no deploy.sdk")
endif()
set(WHISPERRU_CONTRACT "${WHISPERRU_SDK}")
if(NOT EXISTS "${WHISPERRU_CONTRACT}/speechbroker-voice-model.h")
    set(WHISPERRU_CONTRACT "${CMAKE_CURRENT_SOURCE_DIR}/../adapter-voice/contract")
    if(EXISTS "${WHISPERRU_CONTRACT}/speechbroker-voice-model.h")
        message(STATUS
            "The installed SDK of the adapter has no speechbroker-voice-model.h (${WHISPERRU_SDK}).\n"
            "   Building against the contract in the repository instead: ${WHISPERRU_CONTRACT}\n"
            "   Lay the adapter out to build the way a third party would.")
    else()
        message(FATAL_ERROR
            "The contract of the adapter was not found: ${WHISPERRU_SDK}/speechbroker-voice-model.h\n"
            "Lay the voice adapter out first - it publishes the contract as its own SDK package.")
    endif()
endif()
message(STATUS "Speech Broker voice-model contract: ${WHISPERRU_CONTRACT}")

# ---------------------------------------------------------------- dependencies
# CommonLibSSE-NG looks for fmt and spdlog through find_package, so we slip them
# in with FetchContent and OVERRIDE_FIND_PACKAGE. vcpkg is not needed for this.
# The versions are the adapter's, deliberately: two SKSE plugins of one mod that
# disagree about spdlog is a class of bug nobody should have to diagnose twice.
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
# of a CSV, and that is exactly the path by which VR is supported.
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

# ------------------------------------------------------------------ our plugin
# Gathered rather than listed, and CONFIGURE_DEPENDS is what makes that honest: a
# new file under src/ is picked up by the next build without anybody having to
# remember this line. The headers are named for the same reason the sources are:
# they compile nothing, but a header that is in the target shows up in the
# solution and is searched by the IDE, and a header nobody can find is a header
# nobody reads.
file(GLOB_RECURSE SHIM_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
file(GLOB_RECURSE SHIM_HEADERS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h")
add_library(SpeechBrokerModelWhisperRu SHARED ${SHIM_SOURCES} ${SHIM_HEADERS})

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/src" FILES ${SHIM_SOURCES} ${SHIM_HEADERS})

if(MSVC)
    target_compile_options(SpeechBrokerModelWhisperRu PRIVATE
        /MP /W4 /WX /permissive- /Zc:preprocessor /EHsc /utf-8
    )
endif()

set_target_properties(SpeechBrokerModelWhisperRu PROPERTIES
    OUTPUT_NAME "SpeechBrokerModelWhisperRu" SUFFIX ".dll")

target_compile_definitions(SpeechBrokerModelWhisperRu PRIVATE
    PLUGIN_NAME="SpeechBrokerModelWhisperRu"
    PLUGIN_AUTHOR="Vhodnoylogin"
    PLUGIN_VERSION="${PROJECT_VERSION}"
)

target_include_directories(SpeechBrokerModelWhisperRu PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
    "${WHISPERRU_CONTRACT}"
    "${RAPIDCSV_INCLUDE_DIRS}"
)

target_link_libraries(SpeechBrokerModelWhisperRu PRIVATE
    CommonLibSSE::CommonLibSSE
    nlohmann_json::nlohmann_json
)
