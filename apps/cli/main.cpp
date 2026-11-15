#include "exchange/client/cli.hpp"

#include <unistd.h>

#include <charconv>
#include <cstdlib>
#include <iostream>
#include <span>
#include <string_view>
#include <system_error>

namespace {

constexpr std::string_view usage =
    "Usage: exchange-cli [--address ADDRESS] [--port PORT]\n"
    "\n"
    "Connects to an exchange server and reads commands from standard input.\n"
    "Type 'help' once connected for the list of commands.\n"
    "\n"
    "  --address ADDRESS  IPv4 address of the server (default 127.0.0.1)\n"
    "  --port PORT        TCP port of the server (default 9000)\n";

bool parse_port(std::string_view text, std::uint16_t& port) {
    const char* const begin = text.data();
    const char* const end = begin + text.size();
    const auto [parsed_end, error] = std::from_chars(begin, end, port);
    return error == std::errc{} && parsed_end == end;
}

} // namespace

int main(int argc, char** argv) {
    const std::span<char*> arguments(argv, static_cast<std::size_t>(argc));
    exchange::client::CliOptions options;

    for (std::size_t i = 1; i < arguments.size(); i += 2) {
        const std::string_view name = arguments[i];
        if (name == "-h" || name == "--help") {
            std::cout << usage;
            return EXIT_SUCCESS;
        }
        const bool has_value = i + 1 < arguments.size();
        if (name == "--address" && has_value) {
            options.address = arguments[i + 1];
        } else if (name == "--port" && has_value && parse_port(arguments[i + 1], options.port)) {
            continue;
        } else {
            std::cerr << "error: invalid argument " << name << "\n\n" << usage;
            return EXIT_FAILURE;
        }
    }
    return exchange::client::run_cli(options, STDIN_FILENO, std::cout);
}
