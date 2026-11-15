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

add_library(exchange_protocol STATIC
    exchange/protocol/codec.cpp)
target_link_libraries(exchange_protocol
    PUBLIC exchange_core
    PRIVATE exchange_warnings)

add_library(exchange_net STATIC
    exchange/net/tcp.cpp)
target_include_directories(exchange_net PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(exchange_net PRIVATE exchange_warnings)

find_package(Threads REQUIRED)

add_library(exchange_server STATIC
    exchange/server/engine_pipeline.cpp
    exchange/server/server.cpp)
target_link_libraries(exchange_server
    PUBLIC exchange_core exchange_net exchange_protocol exchange_queue Threads::Threads
    PRIVATE exchange_warnings)

add_library(exchange_client STATIC
    exchange/client/book_view.cpp
    exchange/client/cli.cpp
    exchange/client/cli_session.cpp)
target_link_libraries(exchange_client
    PUBLIC exchange_core exchange_net exchange_protocol
    PRIVATE exchange_warnings)
