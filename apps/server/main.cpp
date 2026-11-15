#include "exchange/server/server.hpp"

#include <charconv>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

namespace {

constexpr std::string_view usage =
    "Usage: exchange-server [--address ADDRESS] [--port PORT]\n"
    "\n"
    "Runs the exchange until interrupted (Ctrl-C).\n"
    "\n"
    "  --address ADDRESS  IPv4 address to listen on (default 127.0.0.1)\n"
    "  --port PORT        TCP port to listen on (default 9000)\n";

// Set before the signal handlers are installed and never changed afterwards.
exchange::server::Server* running_server = nullptr; // NOLINT(*-avoid-non-const-global-variables)

extern "C" void handle_stop_signal(int /*signal*/) {
    if (running_server != nullptr) {
        running_server->request_stop();
    }
}

bool parse_port(std::string_view text, std::uint16_t& port) {
    const char* const begin = text.data();
    const char* const end = begin + text.size();
    const auto [parsed_end, error] = std::from_chars(begin, end, port);
    return error == std::errc{} && parsed_end == end;
}

} // namespace

int main(int argc, char** argv) {
    const std::span<char*> arguments(argv, static_cast<std::size_t>(argc));
    exchange::server::ServerConfig config;

    for (std::size_t i = 1; i < arguments.size(); i += 2) {
        const std::string_view name = arguments[i];
        if (name == "-h" || name == "--help") {
            std::cout << usage;
            return EXIT_SUCCESS;
        }
        const bool has_value = i + 1 < arguments.size();
        if (name == "--address" && has_value) {
            config.address = arguments[i + 1];
        } else if (name == "--port" && has_value && parse_port(arguments[i + 1], config.port)) {
            continue;
        } else {
            std::cerr << "error: invalid argument " << name << "\n\n" << usage;
            return EXIT_FAILURE;
        }
    }

    try {
        exchange::server::Server server(config);
        running_server = &server;

        struct sigaction action{};
        action.sa_handler = handle_stop_signal;
        sigaction(SIGINT, &action, nullptr);
        sigaction(SIGTERM, &action, nullptr);

        std::cout << "exchange-server listening on " << config.address << ':' << server.port()
                  << '\n'
                  << std::flush; // Visible at once, even when the output is a pipe.
        server.run();
        running_server = nullptr;
        std::cout << "exchange-server stopped\n";
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
