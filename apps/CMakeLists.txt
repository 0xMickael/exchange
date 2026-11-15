add_executable(exchange-replay replay/main.cpp)
target_link_libraries(exchange-replay PRIVATE exchange_core exchange_warnings)

add_executable(exchange-server server/main.cpp)
target_link_libraries(exchange-server PRIVATE exchange_server exchange_warnings)
