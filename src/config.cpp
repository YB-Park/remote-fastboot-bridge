#include "config.hpp"
#include "socket.hpp"

#include <algorithm>
#include <charconv>
#include <map>
#include <set>
#include <variant>

namespace proxy {
namespace {
struct Json {
    using Array = std::vector<Json>;
    using Object = std::map<std::string, Json>;
    std::variant<std::string, std::uint64_t, Array, Object> value;

    template<class T>
    const T& get() const {
        const auto* item = std::get_if<T>(&value);
        if (!item) throw Error("unexpected JSON value type");
        return *item;
    }
};

std::uint64_t integer(std::string_view text) {
    std::uint64_t result{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
        throw Error("invalid unsigned integer: " + log_text(text));
    }
    return result;
}

class Parser {
public:
    explicit Parser(std::string_view data) : input_(data) {
        if (input_.starts_with("\xef\xbb\xbf")) input_.remove_prefix(3);
    }

    Json parse() {
        auto result = value(0);
        skip_space();
        if (!input_.empty()) fail();
        return result;
    }

private:
    [[noreturn]] static void fail() { throw Error("invalid configuration JSON syntax"); }

    void skip_space() {
        const auto offset = input_.find_first_not_of(" \t\r\n");
        input_.remove_prefix(std::min(offset, input_.size()));
    }

    bool take(char c) {
        skip_space();
        if (input_.empty() || input_.front() != c) return false;
        input_.remove_prefix(1);
        return true;
    }

    char pop() {
        if (input_.empty()) fail();
        const auto c = input_.front();
        input_.remove_prefix(1);
        return c;
    }

    std::uint32_t code_unit() {
        if (input_.size() < 4) fail();
        std::uint32_t code{};
        const auto parsed = std::from_chars(input_.data(), input_.data() + 4, code, 16);
        if (parsed.ec != std::errc{} || parsed.ptr != input_.data() + 4) fail();
        input_.remove_prefix(4);
        return code;
    }

    void unicode(std::string& result) {
        auto code = code_unit();
        if (code >= 0xd800 && code <= 0xdbff) {
            if (pop() != '\\' || pop() != 'u') fail();
            const auto low = code_unit();
            if (low < 0xdc00 || low > 0xdfff) fail();
            code = 0x10000 + ((code - 0xd800) << 10) + (low - 0xdc00);
        }
        if (code == 0 || (code >= 0xd800 && code <= 0xdfff)) fail();
        const auto continuation = [&](unsigned shift) {
            result += static_cast<char>(0x80 | ((code >> shift) & 0x3f));
        };
        if (code < 0x80) {
            result += static_cast<char>(code);
        } else if (code < 0x800) {
            result += static_cast<char>(0xc0 | (code >> 6));
            continuation(0);
        } else if (code < 0x10000) {
            result += static_cast<char>(0xe0 | (code >> 12));
            continuation(6);
            continuation(0);
        } else {
            result += static_cast<char>(0xf0 | (code >> 18));
            continuation(12);
            continuation(6);
            continuation(0);
        }
    }

    std::string string() {
        if (!take('"')) fail();
        std::string result;
        for (char c = pop(); c != '"'; c = pop()) {
            if (static_cast<unsigned char>(c) < 0x20) fail();
            if (c != '\\') {
                result += c;
                continue;
            }
            switch (pop()) {
                case '"': result += '"'; break;
                case '\\': result += '\\'; break;
                case '/': result += '/'; break;
                case 'b': result += '\b'; break;
                case 'f': result += '\f'; break;
                case 'n': result += '\n'; break;
                case 'r': result += '\r'; break;
                case 't': result += '\t'; break;
                case 'u': unicode(result); break;
                default: fail();
            }
        }
        return result;
    }

    Json value(unsigned depth) {
        if (depth > 8) fail();
        skip_space();
        if (input_.empty()) fail();
        if (input_.front() == '"') return Json{string()};
        if (take('{')) {
            Json::Object result;
            if (!take('}')) {
                do {
                    auto key = string();
                    if (!take(':')) fail();
                    if (!result.emplace(std::move(key), value(depth + 1)).second) {
                        throw Error("duplicate JSON key");
                    }
                } while (take(','));
                if (!take('}')) fail();
            }
            return Json{std::move(result)};
        }
        if (take('[')) {
            Json::Array result;
            if (!take(']')) {
                do { result.push_back(value(depth + 1)); } while (take(','));
                if (!take(']')) fail();
            }
            return Json{std::move(result)};
        }
        const auto count = std::min(input_.find_first_not_of("0123456789"), input_.size());
        if (count == 0 || (count > 1 && input_.front() == '0')) fail();
        const auto result = integer(input_.substr(0, count));
        input_.remove_prefix(count);
        return Json{result};
    }

    std::string_view input_;
};

std::uint64_t bounded(const Json& value, std::uint64_t min, std::uint64_t max) {
    const auto n = value.get<std::uint64_t>();
    if (n < min || n > max) throw Error("numeric configuration value out of range");
    return n;
}

Mapping mapping_from_json(const Json& value) {
    Mapping result;
    for (const auto& [key, item] : value.get<Json::Object>()) {
        if (key == "serial") result.selector.serial = item.get<std::string>();
        else if (key == "serial_aliases") {
            for (const auto& alias : item.get<Json::Array>()) {
                result.selector.aliases.push_back(alias.get<std::string>());
            }
        } else if (key == "location") result.selector.location = item.get<std::string>();
        else if (key == "instance") result.selector.instance = item.get<std::string>();
        else if (key == "port") result.port = static_cast<std::uint16_t>(bounded(item, 1, 65535));
        else throw Error("unknown mapping key: " + log_text(key));
    }
    return result;
}
}

Subnet subnet(std::string_view text) {
    const auto slash = text.find('/');
    const auto bits = slash == text.npos ? 32 : integer(text.substr(slash + 1));
    if (bits > 32) throw Error("invalid IPv4 prefix length");
    const auto mask = bits == 0 ? 0U : 0xffffffffU << static_cast<unsigned>(32 - bits);
    return {ipv4(text.substr(0, slash)) & mask, mask};
}

Config read_config(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw Error("cannot open configuration file");
    std::string json(65537, '\0');
    file.read(json.data(), static_cast<std::streamsize>(json.size()));
    if (file.bad() || file.gcount() >= static_cast<std::streamsize>(json.size())) {
        throw Error("configuration cannot be read or exceeds 64 KiB");
    }
    json.resize(static_cast<std::size_t>(file.gcount()));

    Config config;
    const auto root = Parser(json).parse();
    for (const auto& [key, item] : root.get<Json::Object>()) {
        if (key == "listen_address") config.bind = item.get<std::string>();
        else if (key == "usb_backend") config.backend = item.get<std::string>();
        else if (key == "devices") {
            for (const auto& mapping : item.get<Json::Array>()) {
                config.mappings.push_back(mapping_from_json(mapping));
            }
        } else if (key == "allowlist") {
            for (const auto& cidr : item.get<Json::Array>()) {
                config.allow.push_back(subnet(cidr.get<std::string>()));
            }
        } else if (key == "reconnect_timeout_ms") config.reconnect = Millis(bounded(item, 100, 600000));
        else if (key == "session_timeout_ms") config.idle = Millis(bounded(item, 100, 3600000));
        else if (key == "io_timeout_ms") config.io_timeout = Millis(bounded(item, 100, 600000));
        else if (key == "usb_timeout_ms") config.usb_timeout = Millis(bounded(item, 100, 600000));
        else if (key == "max_sessions") config.max_sessions = bounded(item, 1, 32);
        else if (key == "log_file") config.log = utf8_path(item.get<std::string>());
        else if (key == "log_level") {
            const auto& level = item.get<std::string>();
            const auto index = std::find(std::begin(level_names), std::end(level_names), level);
            if (index == std::end(level_names)) throw Error("log_level must be ERROR, WARN, INFO, DEBUG or TRACE");
            config.level = static_cast<Level>(index - std::begin(level_names));
        } else throw Error("unknown configuration key: " + log_text(key));
    }
    return config;
}

void validate(const Config& config) {
    const auto bind_address = ipv4(config.bind);
    if (config.backend != "auto" && config.backend != "winusb" && config.backend != "mock") {
        throw Error("usb_backend must be auto, winusb or mock");
    }
    if (config.backend == "mock" && (bind_address >> 24) != 127) {
        throw Error("mock backend requires an IPv4 loopback listen address");
    }
    if (config.mappings.empty() || config.mappings.size() > 32) {
        throw Error("configure from 1 to 32 device port mappings");
    }
    std::set<std::uint16_t> ports;
    for (const auto& mapping : config.mappings) {
        const auto& selector = mapping.selector;
        if (mapping.port == 0 || !ports.insert(mapping.port).second) {
            throw Error("zero or duplicate listener port");
        }
        if (selector.serial.empty() && selector.instance.empty()) {
            throw Error("each mapping requires serial or Windows device instance");
        }
        if (!selector.aliases.empty() && (selector.location.empty() || selector.serial.empty() ||
                                          !selector.instance.empty())) {
            throw Error("serial_aliases require serial and location without an instance pin");
        }
        std::set<std::string> serials{selector.serial};
        for (const auto& alias : selector.aliases) {
            if (alias.empty() || !serials.insert(alias).second) throw Error("empty or duplicate serial alias");
        }
    }
}
}
