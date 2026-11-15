#pragma once

#include "exchange/core/command.hpp"
#include "exchange/core/event.hpp"
#include "exchange/core/types.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace exchange {

// One-line text forms of commands and events, used by tests, the replay tool
// and debugging output. Examples:
//
//   new 3 1 buy 102 12
//   fill session=3 order_id=4 price=101 quantity=5 remaining=7
//   book side=sell price=101 total=0

[[nodiscard]] std::string to_string(Side side);
[[nodiscard]] std::string to_string(const Command& command);
[[nodiscard]] std::string to_string(const Event& event);

// Parses the text form of a command, as produced by to_string(const Command&).
// Words may be separated by any number of spaces or tabs. Returns nullopt if
// the line is not a well-formed command.
[[nodiscard]] std::optional<Command> parse_command(std::string_view line);

} // namespace exchange
