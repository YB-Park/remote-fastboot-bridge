#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <mutex>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace proxy {
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using Bytes = std::span<std::uint8_t>;
using ConstBytes = std::span<const std::uint8_t>;
using Cancel = std::function<bool()>;
using Millis = std::chrono::milliseconds;

inline ConstBytes bytes(std::string_view text) {
    return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}
inline std::string_view as_text(ConstBytes data) {
    return {reinterpret_cast<const char*>(data.data()), data.size()};
}

inline std::filesystem::path utf8_path(std::string_view value) {
    return std::filesystem::path(std::u8string(value.begin(), value.end()));
}

class Error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

enum class Level { error, warn, info, debug, trace };
inline constexpr std::string_view level_names[] = {"ERROR", "WARN", "INFO", "DEBUG", "TRACE"};

class Logger {
public:
    Logger(Level level, const std::filesystem::path& path) : level_(level) {
        if (!path.empty()) {
            file_.open(path, std::ios::app);
            if (!file_) throw Error("cannot open log file");
        }
    }

    template<class... Args>
    void write(Level level, const Args&... args) {
        if (level > level_) return;
        const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
        const auto time = std::chrono::system_clock::to_time_t(now);
        std::tm utc{};
#ifdef _WIN32
        gmtime_s(&utc, &time);
#else
        gmtime_r(&time, &utc);
#endif
        char timestamp[32]{};
        std::strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", &utc);
        std::ostringstream line;
        line << timestamp << ' ' << level_names[static_cast<int>(level)] << ' ';
        (line << ... << args);
        std::scoped_lock lock(mutex_);
        std::cerr << line.str() << '\n';
        if (file_.is_open()) {
            file_ << line.str() << '\n';
            file_.flush();
        }
    }

private:
    Level level_;
    std::mutex mutex_;
    std::ofstream file_;
};

inline std::string log_text(std::string_view text, std::size_t max = 128) {
    std::string safe;
    for (const auto c : text.substr(0, max)) {
        const auto ch = static_cast<unsigned char>(c);
        safe += ch >= 0x20 && ch < 0x7f ? c : '?';
    }
    if (text.size() > max) safe += "...";
    return safe;
}
}
