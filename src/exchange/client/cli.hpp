#pragma once

#include <cstdint>
#include <iosfwd>
#include <string>

namespace exchange::client {

struct CliOptions {
    std::string address = "127.0.0.1";
    std::uint16_t port = 9000;
};

// Runs the command-line client: connects to the server, then reads commands
// from the file descriptor `input` and writes what happens to `output`, until
// the user quits, the input ends or the connection is lost.
//
// After each command the client waits for the server's answers before reading
// the next one, so a script given on standard input produces the same
// transcript on every run.
//
// Returns the process exit status: 0 on a normal end, 1 on a connection problem.
int run_cli(const CliOptions& options, int input, std::ostream& output);

} // namespace exchange::client
