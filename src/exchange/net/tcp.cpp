#include "exchange/net/tcp.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

#include <cerrno>
#include <system_error>

namespace exchange::net {

namespace {

[[noreturn]] void throw_errno(const std::string& what) {
    throw std::system_error(errno, std::generic_category(), what);
}

sockaddr_in make_address(const std::string& address, std::uint16_t port) {
    sockaddr_in result{};
    result.sin_family = AF_INET;
    result.sin_port = htons(port);
    if (inet_pton(AF_INET, address.c_str(), &result.sin_addr) != 1) {
        throw std::system_error(EINVAL, std::generic_category(), "not an IPv4 address: " + address);
    }
    return result;
}

// The socket API takes every address type through a pointer to the generic
// sockaddr; this cast is how the API is meant to be called.
const sockaddr* as_sockaddr(const sockaddr_in& address) {
    return reinterpret_cast<const sockaddr*>(&address); // NOLINT(*-reinterpret-cast)
}

sockaddr* as_sockaddr(sockaddr_in& address) {
    return reinterpret_cast<sockaddr*>(&address); // NOLINT(*-reinterpret-cast)
}

} // namespace

FileDescriptor listen_tcp(const std::string& address, std::uint16_t port) {
    const sockaddr_in socket_address = make_address(address, port);

    FileDescriptor socket(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    if (!socket.valid()) {
        throw_errno("socket");
    }
    // Lets a restarted server bind its port again without waiting for old
    // connections to expire.
    const int enable = 1;
    if (::setsockopt(socket.get(), SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable)) != 0) {
        throw_errno("setsockopt(SO_REUSEADDR)");
    }
    if (::bind(socket.get(), as_sockaddr(socket_address), sizeof(socket_address)) != 0) {
        throw_errno("bind " + address + ":" + std::to_string(port));
    }
    if (::listen(socket.get(), SOMAXCONN) != 0) {
        throw_errno("listen");
    }
    return socket;
}

FileDescriptor connect_tcp(const std::string& address, std::uint16_t port) {
    const sockaddr_in socket_address = make_address(address, port);

    FileDescriptor socket(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
    if (!socket.valid()) {
        throw_errno("socket");
    }
    if (::connect(socket.get(), as_sockaddr(socket_address), sizeof(socket_address)) != 0) {
        throw_errno("connect " + address + ":" + std::to_string(port));
    }
    set_tcp_no_delay(socket);
    return socket;
}

std::uint16_t local_port(const FileDescriptor& socket) {
    sockaddr_in address{};
    socklen_t length = sizeof(address);
    if (::getsockname(socket.get(), as_sockaddr(address), &length) != 0) {
        throw_errno("getsockname");
    }
    return ntohs(address.sin_port);
}

void set_tcp_no_delay(const FileDescriptor& socket) {
    const int enable = 1;
    if (::setsockopt(socket.get(), IPPROTO_TCP, TCP_NODELAY, &enable, sizeof(enable)) != 0) {
        throw_errno("setsockopt(TCP_NODELAY)");
    }
}

} // namespace exchange::net
