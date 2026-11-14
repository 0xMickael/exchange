cmake_minimum_required(VERSION 3.25)

project(exchange
    VERSION 0.1.0
    DESCRIPTION "Educational limit order matching engine"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(EXCHANGE_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
set(EXCHANGE_SANITIZER "" CACHE STRING "Sanitizer to build with: empty, 'address' (ASan + UBSan) or 'thread' (TSan)")
set_property(CACHE EXCHANGE_SANITIZER PROPERTY STRINGS "" address thread)

# Sanitizer flags are global on purpose: every object linked into a binary,
# including GoogleTest, must be instrumented for the reports to be reliable.
if(EXCHANGE_SANITIZER STREQUAL "address")
    add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
elseif(EXCHANGE_SANITIZER STREQUAL "thread")
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
    add_link_options(-fsanitize=thread)
elseif(NOT EXCHANGE_SANITIZER STREQUAL "")
    message(FATAL_ERROR "Unknown EXCHANGE_SANITIZER value: '${EXCHANGE_SANITIZER}'")
endif()

# Warnings are attached to a target instead of being global, so that they apply
# to this project's code only and not to third-party code.
add_library(exchange_warnings INTERFACE)
target_compile_options(exchange_warnings INTERFACE
    -Wall
    -Wextra
    -Wpedantic
    -Wconversion
    -Wsign-conversion
    -Wshadow
    $<$<BOOL:${EXCHANGE_WARNINGS_AS_ERRORS}>:-Werror>)

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
