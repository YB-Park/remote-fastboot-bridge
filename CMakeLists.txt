cmake_minimum_required(VERSION 3.21)
project(RemoteFastbootProxy VERSION 0.1.0 LANGUAGES CXX)

include(CTest)
option(FASTBOOT_PROXY_MOCK "Build the loopback-only development backend" ${BUILD_TESTING})
set(FASTBOOT_CLIENT "" CACHE FILEPATH "Google Platform Tools fastboot executable for integration tests")
find_package(Threads REQUIRED)

add_library(proxy_core
    src/config.cpp
    src/devices.cpp
    src/protocol.cpp
    src/socket.cpp
)
target_compile_features(proxy_core PUBLIC cxx_std_20)
set_target_properties(proxy_core PROPERTIES CXX_EXTENSIONS OFF)
target_include_directories(proxy_core PUBLIC src)
target_link_libraries(proxy_core PUBLIC Threads::Threads)
target_compile_definitions(proxy_core PUBLIC PROXY_VERSION="${PROJECT_VERSION}")

if(WIN32)
    target_sources(proxy_core PRIVATE src/winusb.cpp)
    target_compile_definitions(proxy_core PUBLIC UNICODE _UNICODE _WIN32_WINNT=0x0a00
        WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(proxy_core PUBLIC ws2_32 setupapi cfgmgr32 winusb ole32 advapi32)
endif()
if(MSVC)
    set_property(TARGET proxy_core PROPERTY MSVC_RUNTIME_LIBRARY
        "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(proxy_core PUBLIC /W4 /permissive- /utf-8)
else()
    target_compile_options(proxy_core PUBLIC -Wall -Wextra -Wpedantic)
endif()
if(FASTBOOT_PROXY_MOCK)
    target_sources(proxy_core PRIVATE src/mock.cpp)
    target_compile_definitions(proxy_core PUBLIC PROXY_MOCK)
endif()

add_executable(fastboot-proxy src/main.cpp src/server.cpp)
target_link_libraries(fastboot-proxy PRIVATE proxy_core)
if(MINGW)
    target_link_options(fastboot-proxy PRIVATE -municode)
endif()
if(MSVC)
    set_property(TARGET fastboot-proxy PROPERTY MSVC_RUNTIME_LIBRARY
        "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
install(TARGETS fastboot-proxy RUNTIME DESTINATION .)
install(FILES README.md LICENSE THIRD_PARTY_NOTICES.md config.example.json DESTINATION .)
install(DIRECTORY docs/ DESTINATION docs FILES_MATCHING PATTERN "*.md")

if(BUILD_TESTING)
    add_executable(proxy-unit tests/unit.cpp)
    target_link_libraries(proxy-unit PRIVATE proxy_core)
    if(MSVC)
        set_property(TARGET proxy-unit PROPERTY MSVC_RUNTIME_LIBRARY
            "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    add_test(NAME unit COMMAND proxy-unit)
    if(FASTBOOT_PROXY_MOCK)
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
        add_test(NAME mock-integration COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration.py" "$<TARGET_FILE:fastboot-proxy>")
        set_tests_properties(mock-integration PROPERTIES TIMEOUT 100)
        if(FASTBOOT_CLIENT)
            add_test(NAME official-client COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration.py" "$<TARGET_FILE:fastboot-proxy>"
                --fastboot "${FASTBOOT_CLIENT}")
            set_tests_properties(official-client PROPERTIES TIMEOUT 100)
        endif()
    endif()
endif()
