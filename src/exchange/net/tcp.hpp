#pragma once

#include "exchange/net/file_descriptor.hpp"

#include <cstdint>
#include <string>

// Small wrappers over the Linux socket calls. Every function throws
// std::system_error, carrying errno, if the operating system refuses.
namespace exchange::net {

// A non-blocking socket listening on an IPv4 address such as "127.0.0.1".
// Port 0 asks the system for any free port; local_port() tells which.
[[nodiscard]] FileDescriptor listen_tcp(const std::string& address, std::uint16_t port);

// A blocking socket connected to a server, with Nagle's algorithm disabled.
[[nodiscard]] FileDescriptor connect_tcp(const std::string& address, std::uint16_t port);

[[nodiscard]] std::uint16_t local_port(const FileDescriptor& socket);

// Disables Nagle's algorithm, which delays small packets in the hope of
// merging them. That delay is the opposite of what a request/response
// protocol with small messages needs.
void set_tcp_no_delay(const FileDescriptor& socket);

} // namespace exchange::net
