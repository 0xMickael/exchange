add_executable(exchange-replay replay/main.cpp)
target_link_libraries(exchange-replay PRIVATE exchange_core exchange_warnings)

add_executable(exchange-server server/main.cpp)
target_link_libraries(exchange-server PRIVATE exchange_server exchange_warnings)

add_executable(exchange-cli cli/main.cpp)
target_link_libraries(exchange-cli PRIVATE exchange_client exchange_warnings)
