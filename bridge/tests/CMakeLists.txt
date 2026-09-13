# Консольный хост проверки: то же ядро, но без игры.
#
# Цель нарочно НЕ связана с CommonLibSSE. Это и есть сторож правила «ядро
# не знает об игре»: стоит включению <SKSE/...> или <RE/...> снова появиться
# в src/core или src/bus, и хост перестанет собираться. Нарушить правило
# молча станет нельзя.
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
