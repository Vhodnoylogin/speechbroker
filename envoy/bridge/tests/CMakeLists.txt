# The console host for checks: the same core, but without the game.
#
# The target is deliberately NOT linked against CommonLibSSE. That is the guard
# of the rule "the core knows nothing about the game": let an include of
# <SKSE/...> or <RE/...> appear again in src/core or src/bus and the host stops
# building. Breaking the rule silently becomes impossible.
add_executable(envoy-host host.cpp)

target_link_libraries(envoy-host PRIVATE envoy-core)

target_compile_definitions(envoy-host PRIVATE
    ENVOY_TEST_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)

if(MSVC)
    target_compile_options(envoy-host PRIVATE
        /MP /W4 /WX /permissive- /Zc:preprocessor /EHsc /utf-8
        /wd4068 /wd4189 /wd4200 /wd4201 /wd4324
    )
endif()
