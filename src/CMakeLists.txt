add_library(exchange_core STATIC
    exchange/core/matching_engine.cpp
    exchange/core/order_book.cpp
    exchange/core/replay.cpp
    exchange/core/text.cpp)
target_include_directories(exchange_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(exchange_core PRIVATE exchange_warnings)

# Header-only.
add_library(exchange_queue INTERFACE)
target_include_directories(exchange_queue INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
