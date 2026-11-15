add_executable(exchange-replay replay/main.cpp)
target_link_libraries(exchange-replay PRIVATE exchange_core exchange_warnings)
