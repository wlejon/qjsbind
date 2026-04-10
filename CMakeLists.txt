cmake_minimum_required(VERSION 3.24)
project(qjsbind LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/MP /permissive- /utf-8)
    if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
endif()

# Find QuickJS if not already available
if(NOT TARGET qjs)
    set(QJS_DIR "${CMAKE_SOURCE_DIR}/../brokit/third_party/quickjs" CACHE PATH
        "Path to QuickJS source (default: sibling brokit)")
    if(EXISTS "${QJS_DIR}/CMakeLists.txt")
        add_subdirectory("${QJS_DIR}" "${CMAKE_BINARY_DIR}/quickjs" EXCLUDE_FROM_ALL)
    elseif(EXISTS "${CMAKE_SOURCE_DIR}/third_party/quickjs/CMakeLists.txt")
        add_subdirectory(third_party/quickjs EXCLUDE_FROM_ALL)
    endif()
endif()

# Header-only library
add_library(qjsbind INTERFACE)
target_include_directories(qjsbind INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
if(TARGET qjs)
    target_link_libraries(qjsbind INTERFACE qjs)
endif()

# Tests (standalone builds only)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR AND TARGET qjs)
    enable_testing()
    add_subdirectory(tests)
endif()
