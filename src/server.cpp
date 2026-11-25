#include "server.hpp"
#include "protocol.hpp"

#include <algorithm>
#include <condition_variable>

namespace proxy {
namespace {
struct Client {
    Socket socket;
    std::uint32_t ip;
    Clock::time_point arrival = Clock::now();
};

class Listener {
public:
    Listener(const Config& config, const Mapping& mapping, Devices& devices, Logger& log,
             const std::atomic_bool& stop, std::atomic_size_t& sessions, std::atomic_bool& failed)
        : config_(config), mapping_(mapping), devices_(devices), log_(log), stop_(stop),
          sessions_(sessions), failed_(failed), listener_(Socket::listen(config.bind, mapping.port)) {}

    void start() {
        worker_ = std::jthread([this](std::stop_token stop) { work(stop); });
        acceptor_ = std::jthread([this](std::stop_token stop) { accept(stop); });
        log_.write(Level::info, "listen ", config_.bind, ':', mapping_.port,
                   " serial=", log_text(mapping_.selector.serial),
                   " location=", log_text(mapping_.selector.location));
    }

private:
    bool stopped(std::stop_token token) const {
        return token.stop_requested() || stop_.load() || failed_.load();
    }

    void accept(std::stop_token token) {
        const Cancel stop = [&] { return stopped(token); };
        while (!stop()) {
            try {
                std::uint32_t ip{};
                auto client = listener_.accept(stop, ip);
                if (!client) continue;
                if (!config_.allow.empty() && std::ranges::none_of(config_.allow, [&](const Subnet& rule) {
                        return rule.contains(ip);
                    })) {
                    log_.write(Level::warn, "TCP allowlist rejected ", format_ip(ip), " port=", mapping_.port);
                    continue;
                }
                std::scoped_lock lock(mutex_);
                if (busy_ || sessions_.fetch_add(1) >= config_.max_sessions) {
                    if (!busy_) --sessions_;
                    log_.write(Level::debug, "TCP session limit port=", mapping_.port);
                    continue;
                }
                busy_ = true;
                pending_.emplace(Client{std::move(*client), ip});
                next_.notify_one();
            } catch (const std::exception& error) {
                log_.write(Level::error, "listener port=", mapping_.port, ' ', error.what());
                failed_.store(true);
            }
        }
    }

    void work(std::stop_token token) {
        const Cancel stop = [&] { return stopped(token); };
        for (;;) {
            std::optional<Client> client;
            {
                std::unique_lock lock(mutex_);
                next_.wait_for(lock, token, 200ms, [&] { return pending_.has_value() || stop(); });
                if (stop() && !pending_) return;
                client.swap(pending_);
            }
            if (!client) continue;
            try {
                session(*client, stop);
            } catch (const std::exception& error) {
                log_.write(Level::debug, "session rejected port=", mapping_.port, ' ', error.what());
            }
            client.reset();
            {
                std::scoped_lock lock(mutex_);
                busy_ = false;
                --sessions_;
            }
        }
    }

    void session(Client& client, const Cancel& stop) {
        std::array<std::uint8_t, 4> hello{};
        const auto deadline = client.arrival + 1500ms;
        client.socket.read_exact(hello, std::min(1200ms, config_.io_timeout), stop);
        const auto version = negotiate(as_text(hello));
        auto lease = devices_.acquire(mapping_.selector, binding_, config_.usb_timeout);
        if (!lease) throw Error("mapped USB device offline");
        if (Clock::now() >= deadline) throw Error("USB acquisition missed TCP handshake deadline");
        client.socket.write_all(bytes("FB01"), 300ms, stop);
        log_.write(Level::info, "TCP client=", format_ip(client.ip), " port=", mapping_.port,
                   " serial=", log_text(lease->device().serial), " FB0", version);
        log_.write(Level::trace, "USB transport ", lease->usb().details());

        Transaction transaction;
        const Cancel disconnected = [&] { return stop() || lease->gone(); };
        try {
            bridge(client.socket, lease->usb(), config_, log_, transaction, disconnected);
        } catch (const std::exception& error) {
            if (transaction.phase != Phase::command) {
                lease->quarantine();
                log_.write(Level::error, "port=", mapping_.port, " serial=", log_text(lease->device().serial),
                           " command outcome unknown; USB quarantined until disconnect: ", error.what());
            } else {
                log_.write(Level::debug, "TCP session ended port=", mapping_.port, ' ', error.what());
            }
        }
        if (!transaction.transition) return;
        client.socket.close_after_write();
        const auto started = Clock::now();
        const auto expires = started + config_.reconnect;
        log_.write(Level::info, "awaiting USB departure port=", mapping_.port);
        while (!stop() && !lease->gone() && Clock::now() < expires) std::this_thread::sleep_for(25ms);
        if (!lease->gone()) {
            lease->quarantine();
            log_.write(Level::warn, "USB departure unconfirmed; port quarantined port=", mapping_.port);
        } else {
            log_.write(Level::info, "USB departure confirmed port=", mapping_.port, " wait_ms=",
                       std::chrono::duration_cast<Millis>(Clock::now() - started).count());
        }
    }

    const Config& config_;
    const Mapping& mapping_;
    Devices& devices_;
    Logger& log_;
    const std::atomic_bool& stop_;
    std::atomic_size_t& sessions_;
    std::atomic_bool& failed_;
    Socket listener_;
    Binding binding_;
    std::mutex mutex_;
    std::condition_variable_any next_;
    std::optional<Client> pending_;
    bool busy_ = false;
    std::jthread worker_;
    std::jthread acceptor_;
};
}

void serve(const Config& config, Backend& backend, Logger& log, const std::atomic_bool& stop) {
    validate(config);
    Devices devices(backend, log);
    std::atomic_size_t sessions{0};
    std::atomic_bool failed{false};
    std::vector<std::unique_ptr<Listener>> ports;
    for (const auto& mapping : config.mappings) {
        ports.push_back(std::make_unique<Listener>(config, mapping, devices, log, stop, sessions, failed));
    }
    log.write(Level::info, "fastboot-proxy ", PROXY_VERSION, " backend=", backend.name());
    for (const auto& listener : ports) listener->start();
    while (!stop.load() && !failed.load()) std::this_thread::sleep_for(100ms);
    ports.clear();
    log.write(Level::info, "server stopped");
    if (failed) throw Error("TCP listener failed");
}
}
