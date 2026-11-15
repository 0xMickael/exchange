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
    "Usage: exchange-server [OPTIONS]\n"
    "\n"
    "Runs the exchange until interrupted (Ctrl-C).\n"
    "\n"
    "  --address ADDRESS          IPv4 address to listen on (default 127.0.0.1)\n"
    "  --port PORT                TCP port to listen on (default 9000)\n"
    "  --mode pipeline|inline     pipeline: the matching engine has its own thread,\n"
    "                             connected to the network thread by queues (default)\n"
    "                             inline: one thread does everything\n"
    "  --engine-wait backoff|spin what the engine thread does when idle (pipeline mode)\n"
    "                             backoff: sleeps in short naps (default)\n"
    "                             spin: never sleeps, and uses a whole core\n";

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

bool parse_mode(std::string_view text, exchange::server::ServerMode& mode) {
    if (text == "inline") {
        mode = exchange::server::ServerMode::Inline;
        return true;
    }
    if (text == "pipeline") {
        mode = exchange::server::ServerMode::Pipeline;
        return true;
    }
    return false;
}

bool parse_wait(std::string_view text, exchange::server::EngineWait& wait) {
    if (text == "backoff") {
        wait = exchange::server::EngineWait::Backoff;
        return true;
    }
    if (text == "spin") {
        wait = exchange::server::EngineWait::Spin;
        return true;
    }
    return false;
}

// Stores the value of one option. Returns false if the option or its value is not valid.
bool apply_option(std::string_view name, std::string_view value,
                  exchange::server::ServerConfig& config) {
    if (name == "--address") {
        config.address = value;
        return true;
    }
    if (name == "--port") {
        return parse_port(value, config.port);
    }
    if (name == "--mode") {
        return parse_mode(value, config.mode);
    }
    if (name == "--engine-wait") {
        return parse_wait(value, config.pipeline.wait);
    }
    return false;
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
        if (i + 1 >= arguments.size() || !apply_option(name, arguments[i + 1], config)) {
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

        std::cout << "exchange-server ("
                  << (config.mode == exchange::server::ServerMode::Inline ? "inline" : "pipeline")
                  << " mode) listening on " << config.address << ':' << server.port() << '\n'
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
