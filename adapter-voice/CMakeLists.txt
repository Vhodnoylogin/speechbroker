cmake_minimum_required(VERSION 3.24)

# Тестовый адаптер Envoy: обычный плагин SKSE, то есть мод. С мостом говорит
# вызовом функции внутри процесса игры; наружу, к голосовой службе, ходит сам
# по HTTP. Собирается отдельно от моста и о его исходниках ничего не знает -
# из моста берётся только контракт, и берётся он из установленного мода, папки
# SDK. Путь к ней задаётся в config/build.json ключом sdk.
project(EnvoyVoiceAdapter VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_SKIP_INSTALL_RULES ON)

# ---------------------------------------------------------------- контракт
# Где лежит SDK моста - настройка, а не константа: читаем её из того же файла,
# которым пользуются скрипты раскладки.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/config/build.json" ENVOY_BUILD_JSON)
string(JSON ENVOY_SDK GET "${ENVOY_BUILD_JSON}" deploy sdk)
if(NOT EXISTS "${ENVOY_SDK}/envoy-adapter.h")
    message(FATAL_ERROR
        "Контракт моста не найден: ${ENVOY_SDK}/envoy-adapter.h\n"
        "Сначала выложи мост - он публикует папку SDK внутри своего мода.")
endif()
message(STATUS "Контракт Envoy: ${ENVOY_SDK}")

# ---------------------------------------------------------------- зависимости
# CommonLibSSE-NG ищет fmt и spdlog через find_package, поэтому подсовываем их
# FetchContent'ом с OVERRIDE_FIND_PACKAGE. vcpkg при этом не нужен.
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

# rapidcsv нужен самой CommonLibSSE-NG: из CSV она читает адресную библиотеку,
# и именно этим путём поддерживается VR. Забираем только заголовок.
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
message(STATUS "Fetching CommonLibSSE-NG (первый раз это долго)...")
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

# ---------------------------------------------------------------- наш плагин
file(GLOB_RECURSE ADAPTER_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
add_library(EnvoyVoiceAdapter SHARED ${ADAPTER_SOURCES})

if(MSVC)
    target_compile_options(EnvoyVoiceAdapter PRIVATE
        /MP /W4 /WX /permissive- /Zc:preprocessor /EHsc /utf-8
        /wd4068 /wd4189 /wd4200 /wd4201 /wd4324
    )
endif()

set_target_properties(EnvoyVoiceAdapter PROPERTIES OUTPUT_NAME "EnvoyVoiceAdapter" SUFFIX ".dll")

target_compile_definitions(EnvoyVoiceAdapter PRIVATE
    PLUGIN_NAME="EnvoyVoiceAdapter"
    PLUGIN_AUTHOR="Vhodnoylogin"
    PLUGIN_VERSION="${PROJECT_VERSION}"
)

target_include_directories(EnvoyVoiceAdapter PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${ENVOY_SDK}"
    "${RAPIDCSV_INCLUDE_DIRS}"
)

target_link_libraries(EnvoyVoiceAdapter PRIVATE
    CommonLibSSE::CommonLibSSE
    nlohmann_json::nlohmann_json
    httplib::httplib
)
