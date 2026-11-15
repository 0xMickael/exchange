#include "exchange/core/replay.hpp"

#include "exchange/core/matching_engine.hpp"
#include "exchange/core/text.hpp"

#include <istream>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace exchange {

namespace {

bool is_ignored(std::string_view line) {
    const std::size_t first = line.find_first_not_of(" \t\r");
    return first == std::string_view::npos || line[first] == '#';
}

} // namespace

ReplayResult replay(std::istream& input, std::ostream& output) {
    MatchingEngine engine;
    std::vector<Event> events;
    ReplayResult result;

    std::string line;
    std::size_t line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (is_ignored(line)) {
            continue;
        }
        const std::optional<Command> command = parse_command(line);
        if (!command.has_value()) {
            result.error_line = line_number;
            return result;
        }

        events.clear();
        engine.process(*command, events);

        output << "> " << to_string(*command) << '\n';
        for (const Event& event : events) {
            output << to_string(event) << '\n';
        }
        ++result.commands;
        result.events += events.size();
    }
    return result;
}

} // namespace exchange
