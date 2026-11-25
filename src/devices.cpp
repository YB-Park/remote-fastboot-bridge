#include "devices.hpp"

#include <algorithm>

namespace proxy {
#ifdef _WIN32
std::unique_ptr<Backend> make_winusb();
#endif
#ifdef PROXY_MOCK
std::unique_ptr<Backend> make_mock();
#endif

std::unique_ptr<Backend> make_backend(std::string_view name) {
#ifdef PROXY_MOCK
    if (name == "mock") return make_mock();
#endif
#ifdef _WIN32
    if (name == "auto" || name == "winusb") return make_winusb();
#endif
    throw Error("USB backend unavailable in this build: " + log_text(name));
}

namespace {
bool equal_pnp(std::string_view a, std::string_view b) {
    const auto upper = [](char c) { return c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c; };
    return std::equal(a.begin(), a.end(), b.begin(), b.end(),
                      [&](char x, char y) { return upper(x) == upper(y); });
}
}

std::string device_key(const Device& device) {
    std::string key = device.location.empty() ? "instance:" + device.instance
                                              : "location:" + device.location;
    for (auto& c : key) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    return key;
}

bool matches(const Device& device, const Selector& selector) {
    if (!selector.instance.empty() && !equal_pnp(selector.instance, device.instance)) return false;
    if (!selector.location.empty() && !equal_pnp(selector.location, device.location)) return false;
    if (selector.serial.empty()) return true;
    const auto serial_matches = [&](std::string_view serial) {
        return device.service == "WINUSB" ? equal_pnp(serial, device.serial) : serial == device.serial;
    };
    return serial_matches(selector.serial) || std::ranges::any_of(selector.aliases, serial_matches);
}

std::optional<Device> unique_device(const std::vector<Device>& devices, const Selector& selector,
                                    std::string_view pinned_location) {
    std::optional<Device> found;
    for (const auto& device : devices) {
        if (!matches(device, selector)) continue;
        if (!pinned_location.empty() && !equal_pnp(pinned_location, device.location)) continue;
        if (found) throw Error("ambiguous device selector; configure its USB location or device instance");
        found = device;
    }
    return found;
}

Devices::Devices(Backend& backend, Logger& logger) : backend_(backend), logger_(logger) {
    refresh();
    monitor_ = std::jthread([this](std::stop_token stop) { run(stop); });
}

void Devices::run(std::stop_token stop) {
    bool error_reported = false;
    while (!stop.stop_requested()) {
        std::this_thread::sleep_for(200ms);
        try {
            refresh();
            error_reported = false;
        } catch (const std::exception& error) {
            if (!error_reported) logger_.write(Level::warn, "USB enumeration: ", error.what());
            error_reported = true;
        }
    }
}

void Devices::refresh() {
    auto next = backend_.enumerate();
    std::scoped_lock lock(mutex_);
    for (auto& device : next) {
        const auto old = std::find_if(devices_.begin(), devices_.end(), [&](const Device& value) {
            return value.instance == device.instance && value.path == device.path && value.serial == device.serial;
        });
        if (old == devices_.end()) {
            device.generation = ++generation_;
            logger_.write(Level::info, "USB attached serial=", log_text(device.serial),
                          " instance=", log_text(device.instance));
        } else {
            device.generation = old->generation;
        }
    }
    for (const auto& previous : devices_) {
        if (std::ranges::none_of(next, [&](const Device& device) {
                return device.generation == previous.generation;
            })) {
            logger_.write(Level::info, "USB detached serial=", log_text(previous.serial),
                          " instance=", log_text(previous.instance));
        }
    }
    devices_ = std::move(next);
    std::erase_if(locks_, [&](const auto& pair) {
        if (pair.second.active) return false;
        if (!pair.second.blocked_generation) return true;
        return std::ranges::none_of(devices_, [&](const Device& device) {
            return device.generation == *pair.second.blocked_generation;
        });
    });
}

bool Devices::present(const Device& device) const {
    std::scoped_lock lock(mutex_);
    return std::ranges::any_of(devices_, [&](const Device& item) {
        return item.generation == device.generation;
    });
}

std::unique_ptr<Devices::Lease> Devices::acquire(const Selector& selector, Binding& binding,
                                                 Millis timeout) {
    std::optional<Device> candidate;
    {
        std::scoped_lock lock(mutex_);
        candidate = unique_device(devices_, selector, binding.location);
        if (!candidate) return nullptr;
        if (binding.identity_without_serial && candidate->generation != binding.last_generation) {
            throw Error("device without a configured serial reappeared; verify identity and restart");
        }
        auto& state = locks_[device_key(*candidate)];
        if (state.active) throw Error("USB device is already in use");
        if (state.blocked_generation) throw Error("USB device quarantined; reset and reconnect it");
        state.active = true;
    }
    try {
        auto usb = backend_.open(*candidate, timeout);
        if (usb->removed() || !present(*candidate)) throw Error("USB device changed while opening");
        auto lease = std::make_unique<Lease>(*this, *candidate, std::move(usb));
        binding.location = candidate->location;
        binding.last_generation = candidate->generation;
        binding.identity_without_serial = selector.serial.empty() || candidate->serial.empty();
        return lease;
    } catch (...) {
        release(*candidate, false, false);
        throw;
    }
}

void Devices::release(const Device& device, bool quarantined, bool removed) {
    std::scoped_lock lock(mutex_);
    if (removed) {
        std::erase_if(devices_, [&](const Device& candidate) { return candidate.generation == device.generation; });
    }
    const auto key = device_key(device);
    if (quarantined && !removed) locks_[key] = Lock{false, device.generation};
    else locks_.erase(key);
}

Devices::Lease::Lease(Devices& owner, Device device, std::unique_ptr<Usb> usb)
    : owner_(owner), device_(std::move(device)), usb_(std::move(usb)) {}

Devices::Lease::~Lease() {
    const bool removed = usb_->removed();
    usb_.reset();
    owner_.release(device_, quarantined_, removed);
}

bool Devices::Lease::gone() const {
    return usb_->removed() || !owner_.present(device_);
}
}
