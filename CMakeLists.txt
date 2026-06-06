cmake_minimum_required(VERSION 3.25)
project(retool LANGUAGES CXX)

set(RETOOL_VERSION "1.0.0" CACHE STRING "retool version")

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/src/version.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/src/version.h
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Windows check
if(NOT WIN32)
    message(FATAL_ERROR "retool only supports Windows.")
endif()

# Third-party dependencies
include(FetchContent)
FetchContent_Declare(
    json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
)
FetchContent_MakeAvailable(json)

# Compiler options
if(MSVC)
    add_compile_options(
        /W4
        /WX
        /permissive-
        /utf-8
        /D_UNICODE
        /DUNICODE
    )
endif()

add_executable(retool
    src/main.cpp
    src/inspect.cpp
    src/inspect.h
    src/copy.cpp
    src/copy.h
    src/dedup.cpp
    src/dedup.h
    src/output.cpp
    src/output.h
    src/volume.cpp
    src/volume.h
    src/util.cpp
    src/util.h
    ${CMAKE_CURRENT_BINARY_DIR}/src/version.h
)

# Include directories
target_include_directories(retool PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}/src
)

# Link Shlwapi for path handling if needed, and standard Win32 libraries
target_link_libraries(retool PRIVATE Shlwapi.lib nlohmann_json::nlohmann_json)

