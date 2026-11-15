# The benchmark logic is a library so that the tests can exercise it.
add_library(exchange_bench_lib STATIC
    clock.cpp
    e2e_benchmark.cpp
    engine_benchmark.cpp
    environment.cpp
    latency.cpp
    queue_benchmark.cpp
    report.cpp
    workload.cpp)
target_include_directories(exchange_bench_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(exchange_bench_lib
    PUBLIC exchange_core exchange_net exchange_protocol exchange_queue exchange_server
    PRIVATE exchange_warnings)

# Recorded in every report, so that a result states how its binary was built.
string(TOUPPER "${CMAKE_BUILD_TYPE}" EXCHANGE_BUILD_TYPE_UPPER)
string(STRIP "${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${EXCHANGE_BUILD_TYPE_UPPER}}" EXCHANGE_EFFECTIVE_FLAGS)
target_compile_definitions(exchange_bench_lib PRIVATE
    EXCHANGE_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
    EXCHANGE_CXX_FLAGS="${EXCHANGE_EFFECTIVE_FLAGS}"
    EXCHANGE_SANITIZER_NAME="${EXCHANGE_SANITIZER}")

add_executable(exchange-bench main.cpp)
target_link_libraries(exchange-bench PRIVATE exchange_bench_lib exchange_warnings)
