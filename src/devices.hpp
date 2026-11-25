#pragma once

#include "config.hpp"

#include <memory>
#include <thread>
#include <unordered_map>

namespace proxy {
struct Device {
    std::string serial;
    std::string instance;
    std::string location;
    std::string service;
    std::uint16_t vid = 0;
    std::uint16_t pid = 0;
    std::wstring path;
    std::uint64_t generation = 0;
};

class Usb {
public:
    virtual ~Usb() = default;
    virtual std::size_t read(Bytes destination, const Cancel& stop) = 0;
    virtual std::size_t write(ConstBytes source, const Cancel& stop) = 0;
    virtual std::size_t out_packet_size() const = 0;
    virtual std::string details() const = 0;
    virtual bool removed() const = 0;
};

class Backend {
public:
    virtual ~Backend() = default;
    virtual std::string name() const = 0;
    virtual std::vector<Device> enumerate() = 0;
    virtual std::unique_ptr<Usb> open(const Device& device, Millis timeout) = 0;
    virtual void doctor(std::ostream& out) const = 0;
};

std::unique_ptr<Backend> make_backend(std::string_view name);
std::string device_key(const Device& device);
bool matches(const Device& device, const Selector& selector);
std::optional<Device> unique_device(const std::vector<Device>& devices, const Selector& selector,
                                    std::string_view pinned_location);

struct Binding {
    std::string location;
    std::uint64_t last_generation = 0;
    bool identity_without_serial = false;
};

class Devices {
public:
    class Lease {
    public:
        Lease(Devices& owner, Device device, std::unique_ptr<Usb> usb);
        ~Lease();
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        bool gone() const;
        void quarantine() { quarantined_ = true; }
        Usb& usb() const { return *usb_; }
        const Device& device() const { return device_; }

    private:
        Devices& owner_;
        Device device_;
        std::unique_ptr<Usb> usb_;
        bool quarantined_ = false;
    };

    Devices(Backend& backend, Logger& logger);
    std::unique_ptr<Lease> acquire(const Selector& selector, Binding& binding, Millis timeout);

private:
    friend class Lease;
    struct Lock {
        bool active = false;
        std::optional<std::uint64_t> blocked_generation;
    };

    void refresh();
    void run(std::stop_token stop);
    bool present(const Device& device) const;
    void release(const Device& device, bool quarantined, bool removed);

    Backend& backend_;
    Logger& logger_;
    mutable std::mutex mutex_;
    std::vector<Device> devices_;
    std::unordered_map<std::string, Lock> locks_;
    std::uint64_t generation_ = 0;
    std::jthread monitor_;
};
}
