include(FetchContent)

# Pinned by archive checksum so that every build uses exactly the same sources.
FetchContent_Declare(googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.18.0.tar.gz
    URL_HASH SHA256=6e3191c1455468b3fc35a417fb565c1c5071aee1b7e7f85e30cf48a98d37d8b5
    SYSTEM)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

include(GoogleTest)

add_executable(exchange_tests
    core/matching_engine_test.cpp
    core/order_book_test.cpp)
target_include_directories(exchange_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(exchange_tests PRIVATE
    exchange_core
    exchange_warnings
    GTest::gtest_main)

gtest_discover_tests(exchange_tests)
