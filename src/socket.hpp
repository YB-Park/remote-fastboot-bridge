#pragma once

#include "common.hpp"

#ifdef _WIN32
#include <winsock2.h>
#else
#include <sys/socket.h>
#endif

#include <optional>
#include <utility>

namespace proxy {
#ifdef _WIN32
using NativeSocket = SOCKET;
inline constexpr auto invalid_socket = INVALID_SOCKET;
#else
using NativeSocket = int;
inline constexpr auto invalid_socket = -1;
#endif

class Network {
public:
    Network();
    ~Network();
    Network(const Network&) = delete;
    Network& operator=(const Network&) = delete;
};

class Socket {
public:
    Socket() = default;
    explicit Socket(NativeSocket socket) : socket_(socket) {}
    ~Socket();
    Socket(Socket&& other) noexcept : socket_(std::exchange(other.socket_, invalid_socket)) {}
    Socket& operator=(Socket&& other) noexcept;

    static Socket listen(std::string_view address, std::uint16_t port);
    std::optional<Socket> accept(const Cancel& stop, std::uint32_t& peer) const;
    void read_exact(Bytes destination, Millis timeout, const Cancel& stop) const;
    void write_all(ConstBytes source, Millis timeout, const Cancel& stop) const;
    bool peer_closed() const;
    bool close_after_write();

private:
    void close();
    bool ready(bool write, Clock::time_point deadline, const Cancel& stop) const;
    NativeSocket socket_ = invalid_socket;
};

std::uint32_t ipv4(std::string_view address);
std::string format_ip(std::uint32_t address);
}
