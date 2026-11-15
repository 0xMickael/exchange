#pragma once

#include <cstddef>
#include <iosfwd>

namespace exchange {

struct ReplayResult {
    std::size_t commands = 0;
    std::size_t events = 0;
    // Number (starting at 1) of the first line that is not a valid command, or
    // 0 if the whole input was processed.
    std::size_t error_line = 0;
};

// Feeds a command script to a new MatchingEngine and writes what happens
// (SPEC section 12).
//
// Input: one command per line, in the text form of text.hpp. Empty lines and
// lines starting with '#' are ignored.
// Output: for each command, the line "> <command>" followed by one line per event.
//
// Processing stops at the first invalid line.
ReplayResult replay(std::istream& input, std::ostream& output);

} // namespace exchange
