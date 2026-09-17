cmake_minimum_required(VERSION 3.24)

# The child of the model mod: a small console program that holds the recogniser
# and nothing else.
#
# IT FETCHES NOTHING AND LINKS AGAINST NOTHING and that is the point of it. No
# CommonLibSSE, no SKSE, no json: it is a Windows executable, the protocol, and
# a backend loaded from a DLL at run time. So it configures and builds in
# seconds on a machine that has never seen the game, which is what makes it
# possible to exercise the recognition path without launching anything in a
# headset.
#
# The one thing it does carry is vendor/whisper.cpp/ - whisper.h and the four
# ggml headers it includes, unmodified, at a pinned build. HEADERS ONLY: there
# is no library to link and none is looked for, because whisper_full takes its
# parameter block BY VALUE and a struct of that size written from memory rather
# than from the real header would put every field at the wrong offset without
# saying so. Included as SYSTEM, so somebody else's warnings are not held
# against this build.
#
# The one file it shares with the shim is ../src/core/Wire.cpp - the protocol -
# and it is shared rather than copied because two hand-written parsers of one
# wire format drift, and the drift is silent.
project(SpeechBrokerWhisperChild VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_SKIP_INSTALL_RULES ON)

file(GLOB_RECURSE CHILD_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
file(GLOB_RECURSE CHILD_HEADERS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h")

add_executable(SpeechBrokerWhisperChild
    ${CHILD_SOURCES}
    ${CHILD_HEADERS}
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/core/Wire.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/core/Wire.h"
)

target_include_directories(SpeechBrokerWhisperChild PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/core"
)

target_include_directories(SpeechBrokerWhisperChild SYSTEM PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor/whisper.cpp"
)

if(MSVC)
    # The same wall the shim is built against, and for the same reason: this
    # program runs where nobody is watching, so a warning it was allowed to
    # ignore is a defect nobody ever sees.
    target_compile_options(SpeechBrokerWhisperChild PRIVATE
        /MP /W4 /WX /permissive- /Zc:preprocessor /EHsc /utf-8
    )
endif()

set_target_properties(SpeechBrokerWhisperChild PROPERTIES OUTPUT_NAME "SpeechBrokerWhisperChild")
