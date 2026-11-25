#include "socket.hpp"

#ifdef _WIN32
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <climits>

namespace proxy {
namespace {
#ifdef _WIN32
int socket_error() { return WSAGetLastError(); }
bool retry(int error) { return error == WSAEWOULDBLOCK || error == WSAEINTR; }
constexpr int no_sigpipe = 0;
#else
int socket_error() { return errno; }
bool retry(int error) { return error == EWOULDBLOCK || error == EAGAIN || error == EINTR; }
constexpr int no_sigpipe = MSG_NOSIGNAL;
#endif

[[noreturn]] void fail(std::string_view operation) {
    throw Error(std::string(operation) + " socket error " + std::to_string(socket_error()));
}

void nonblocking(NativeSocket socket) {
#ifdef _WIN32
    u_long mode = 1;
    if (ioctlsocket(socket, FIONBIO, &mode) != 0) fail("ioctlsocket");
#else
    const int flags = fcntl(socket, F_GETFL);
    if (flags == -1 || fcntl(socket, F_SETFL, flags | O_NONBLOCK) == -1) fail("fcntl");
#endif
}

int poll_socket(NativeSocket socket, bool write, int timeout) {
#ifdef _WIN32
    WSAPOLLFD descriptor{socket, static_cast<short>(write ? POLLWRNORM : POLLRDNORM), 0};
    return WSAPoll(&descriptor, 1, timeout);
#else
    pollfd descriptor{socket, static_cast<short>(write ? POLLOUT : POLLIN), 0};
    return poll(&descriptor, 1, timeout);
#endif
}
}

Network::Network() {
#ifdef _WIN32
    WSADATA data{};
    const int error = WSAStartup(MAKEWORD(2, 2), &data);
    if (error != 0) throw Error("WSAStartup error " + std::to_string(error));
#endif
}

Network::~Network() {
#ifdef _WIN32
    WSACleanup();
#endif
}

Socket::~Socket() { close(); }

Socket& Socket::operator=(Socket&& other) noexcept {
    if (this != &other) {
        close();
        socket_ = std::exchange(other.socket_, invalid_socket);
    }
    return *this;
}

void Socket::close() {
    if (socket_ == invalid_socket) return;
#ifdef _WIN32
    closesocket(socket_);
#else
    ::close(socket_);
#endif
    socket_ = invalid_socket;
}

std::uint32_t ipv4(std::string_view address) {
    in_addr parsed{};
    if (inet_pton(AF_INET, std::string(address).c_str(), &parsed) != 1) {
        throw Error("invalid numeric IPv4 address: " + log_text(address));
    }
    return ntohl(parsed.s_addr);
}

std::string format_ip(std::uint32_t address) {
    in_addr value{};
    value.s_addr = htonl(address);
    char buffer[INET_ADDRSTRLEN]{};
    if (!inet_ntop(AF_INET, &value, buffer, sizeof(buffer))) fail("inet_ntop");
    return buffer;
}

Socket Socket::listen(std::string_view address, std::uint16_t port) {
    Socket listener(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (listener.socket_ == invalid_socket) fail("create");
    const int enabled = 1;
#ifdef _WIN32
    constexpr int option = SO_EXCLUSIVEADDRUSE;
#else
    constexpr int option = SO_REUSEADDR;
#endif
    if (setsockopt(listener.socket_, SOL_SOCKET, option,
                   reinterpret_cast<const char*>(&enabled), sizeof(enabled)) != 0) fail("setsockopt");
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(ipv4(address));
    local.sin_port = htons(port);
    if (::bind(listener.socket_, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0) {
        fail("bind " + std::string(address) + ":" + std::to_string(port));
    }
    if (::listen(listener.socket_, 4) != 0) fail("listen");
    nonblocking(listener.socket_);
    return listener;
}

bool Socket::ready(bool write, Clock::time_point deadline, const Cancel& stop) const {
    while (!stop()) {
        const auto now = Clock::now();
        if (now >= deadline) return false;
        const auto timeout = std::clamp(std::chrono::ceil<Millis>(deadline - now), 1ms, 100ms);
        const int result = poll_socket(socket_, write, static_cast<int>(timeout.count()));
        if (result > 0) return !stop();
        if (result < 0 && !retry(socket_error())) fail("poll");
    }
    return false;
}

std::optional<Socket> Socket::accept(const Cancel& stop, std::uint32_t& peer) const {
    if (!ready(false, Clock::now() + 200ms, stop)) return std::nullopt;
    sockaddr_in remote{};
#ifdef _WIN32
    int size = sizeof(remote);
#else
    socklen_t size = sizeof(remote);
#endif
    Socket client(::accept(socket_, reinterpret_cast<sockaddr*>(&remote), &size));
    if (client.socket_ == invalid_socket) {
        if (retry(socket_error())) return std::nullopt;
        fail("accept");
    }
    peer = ntohl(remote.sin_addr.s_addr);
    nonblocking(client.socket_);
    const int enabled = 1;
    if (setsockopt(client.socket_, SOL_SOCKET, SO_KEEPALIVE,
                   reinterpret_cast<const char*>(&enabled), sizeof(enabled)) != 0) fail("keepalive");
    if (setsockopt(client.socket_, IPPROTO_TCP, TCP_NODELAY,
                   reinterpret_cast<const char*>(&enabled), sizeof(enabled)) != 0) fail("TCP_NODELAY");
    return client;
}

void Socket::read_exact(Bytes destination, Millis timeout, const Cancel& stop) const {
    const auto deadline = Clock::now() + timeout;
    while (!destination.empty()) {
        if (!ready(false, deadline, stop)) throw Error("TCP read timeout or cancelled");
        const int size = static_cast<int>(std::min<std::size_t>(destination.size(), INT_MAX));
        const auto count = recv(socket_, reinterpret_cast<char*>(destination.data()), size, 0);
        if (count == 0) throw Error("TCP peer closed");
        if (count < 0) {
            if (retry(socket_error())) continue;
            fail("recv");
        }
        destination = destination.subspan(static_cast<std::size_t>(count));
    }
}

void Socket::write_all(ConstBytes source, Millis timeout, const Cancel& stop) const {
    const auto deadline = Clock::now() + timeout;
    while (!source.empty()) {
        if (!ready(true, deadline, stop)) throw Error("TCP write timeout or cancelled");
        const int size = static_cast<int>(std::min<std::size_t>(source.size(), INT_MAX));
        const auto count = send(socket_, reinterpret_cast<const char*>(source.data()), size, no_sigpipe);
        if (count < 0 && retry(socket_error())) continue;
        if (count <= 0) fail("send");
        source = source.subspan(static_cast<std::size_t>(count));
    }
}

bool Socket::peer_closed() const {
    const int result = poll_socket(socket_, false, 0);
    if (result <= 0) return result < 0 && !retry(socket_error());
    char probe{};
    const auto count = recv(socket_, &probe, 1, MSG_PEEK);
    return count == 0 || (count < 0 && !retry(socket_error()));
}

bool Socket::close_after_write() {
#ifdef _WIN32
    const auto status = shutdown(socket_, SD_SEND);
#else
    const auto status = shutdown(socket_, SHUT_WR);
#endif
    close();
    return status == 0;
}
}
