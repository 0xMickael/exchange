#include "exchange/core/replay.hpp"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <span>
#include <string_view>

namespace {

constexpr std::string_view usage =
    "Usage: exchange-replay [FILE]\n"
    "\n"
    "Feeds a command script to a new matching engine and prints the resulting\n"
    "events. Reads standard input when FILE is omitted.\n"
    "\n"
    "Commands, one per line:\n"
    "  new <session> <client_order_id> <buy|sell> <price> <quantity>\n"
    "  cancel <session> <order_id>\n"
    "  close <session>\n"
    "  snapshot <session>\n";

} // namespace

int main(int argc, char** argv) {
    const std::span<char*> arguments(argv, static_cast<std::size_t>(argc));
    if (arguments.size() > 2) {
        std::cerr << usage;
        return EXIT_FAILURE;
    }
    if (arguments.size() == 2) {
        const std::string_view argument = arguments[1];
        if (argument == "-h" || argument == "--help") {
            std::cout << usage;
            return EXIT_SUCCESS;
        }
    }

    std::ifstream file;
    if (arguments.size() == 2) {
        file.open(arguments[1]);
        if (!file) {
            std::cerr << "error: cannot open " << arguments[1] << '\n';
            return EXIT_FAILURE;
        }
    }
    std::istream& input = arguments.size() == 2 ? file : std::cin;

    const exchange::ReplayResult result = exchange::replay(input, std::cout);
    if (result.error_line != 0) {
        std::cerr << "error: line " << result.error_line << " is not a valid command\n";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
