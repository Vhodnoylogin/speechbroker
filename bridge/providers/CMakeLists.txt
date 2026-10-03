# Every provider is a standalone DLL with the C ABI out of contract/speechbroker-abi.h.
# The bridge loads them from SKSE\Plugins\SpeechBrokerProviders\ and knows nothing about
# them beforehand.
file(GLOB PROVIDER_DIRS RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/*")
foreach(dir ${PROVIDER_DIRS})
    if(IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/${dir}" AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${dir}/CMakeLists.txt")
        add_subdirectory(${dir})
    endif()
endforeach()
