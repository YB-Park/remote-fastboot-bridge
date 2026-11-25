#pragma once

#include "common.hpp"

#include <optional>
#include <vector>

namespace proxy {
struct Selector {
    std::string serial;
    std::vector<std::string> aliases;
    std::string location;
    std::string instance;
};

struct Mapping {
    Selector selector;
    std::uint16_t port = 5554;
};

struct Subnet {
    std::uint32_t address = 0;
    std::uint32_t mask = 0;
    bool contains(std::uint32_t ip) const { return (ip & mask) == address; }
};

struct Config {
    std::string bind = "127.0.0.1";
    std::string backend = "auto";
    std::vector<Subnet> allow;
    std::vector<Mapping> mappings;
    Millis reconnect = 60000ms;
    Millis idle = 60000ms;
    Millis usb_timeout = 90000ms;
    Millis io_timeout = 30000ms;
    std::size_t max_sessions = 8;
    Level level = Level::info;
    std::filesystem::path log;
};

Subnet subnet(std::string_view text);
Config read_config(const std::filesystem::path& path);
void validate(const Config& config);
}
