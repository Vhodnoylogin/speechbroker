# Каждый поставщик - самостоятельная DLL с C-ABI из contract/envoy-abi.h.
# Мост грузит их из SKSE\Plugins\EnvoyProviders\ и ничего о них не знает заранее.
file(GLOB PROVIDER_DIRS RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/*")
foreach(dir ${PROVIDER_DIRS})
    if(IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/${dir}" AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${dir}/CMakeLists.txt")
        add_subdirectory(${dir})
    endif()
endforeach()
