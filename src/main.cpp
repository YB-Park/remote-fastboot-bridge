#include "server.hpp"
#include "socket.hpp"

#include <charconv>
#include <csignal>
#include <iomanip>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {
using namespace proxy;
std::atomic_bool stopped{false};
static_assert(std::atomic_bool::is_always_lock_free);

#ifdef _WIN32
BOOL WINAPI console_event(DWORD event) {
    if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT) return FALSE;
    stopped.store(true);
    return TRUE;
}
#else
void signal_handler(int) { stopped.store(true); }
#endif

void help() {
    std::cout << R"(Remote Fastboot Proxy
  fastboot-proxy --help | --version
  fastboot-proxy devices [--config PATH | --backend auto|winusb|mock]
  fastboot-proxy doctor [--config PATH | --bind IPv4 --port N]
  fastboot-proxy serve --config PATH
  fastboot-proxy serve --serial SERIAL [--location LOCATION] [--port N] [--bind IPv4]
  fastboot-proxy serve --instance INSTANCE [--port N] [--bind IPv4]

Use --allow IPv4[/prefix] repeatedly to limit network peers.
The default listener is 127.0.0.1:5554. Production USB backend: WinUSB.
)";
}

std::uint16_t port_number(std::string_view input) {
    unsigned port{};
    const auto parsed = std::from_chars(input.data(), input.data() + input.size(), port);
    if (parsed.ec != std::errc{} || parsed.ptr != input.data() + input.size() || port == 0 || port > 65535) {
        throw Error("invalid TCP port");
    }
    return static_cast<std::uint16_t>(port);
}

Config options(const std::vector<std::string>& args, std::string_view verb) {
    Config config;
    Mapping mapping;
    if (args.size() == 3 && args[1] == "--config") {
        return read_config(utf8_path(args[2]));
    }
    for (std::size_t i = 1; i < args.size(); i += 2) {
        const auto& flag = args[i];
        if (i + 1 >= args.size()) throw Error("missing value for " + flag);
        const auto& value = args[i + 1];
        if (flag == "--backend") config.backend = value;
        else if (flag == "--bind" && verb != "devices") config.bind = value;
        else if (flag == "--port" && verb != "devices") mapping.port = port_number(value);
        else if (flag == "--serial" && verb == "serve") mapping.selector.serial = value;
        else if (flag == "--location" && verb == "serve") mapping.selector.location = value;
        else if (flag == "--instance" && verb == "serve") mapping.selector.instance = value;
        else if (flag == "--allow" && verb == "serve") config.allow.push_back(subnet(value));
        else throw Error("unknown or incompatible option: " + flag);
    }
    if (verb != "devices") config.mappings.push_back(std::move(mapping));
    return config;
}

void print_device(const Device& device) {
    std::cout << (device.serial.empty() ? "(no serial)" : device.serial)
              << " vid:pid=" << std::hex << std::setfill('0') << std::setw(4) << device.vid
              << ':' << std::setw(4) << device.pid << std::dec
              << " class=ff/42/03 driver=" << (device.service.empty() ? "(none)" : device.service)
              << " path=" << (device.path.empty() ? "unavailable" : "present")
              << "\n  instance=" << device.instance << "\n  location=" << device.location << '\n';
}

int diagnostics(std::string_view verb, const Config& config, Backend& backend) {
    const bool doctor = verb == "doctor";
    bool healthy = true;
    if (doctor) {
        std::cout << "USB backend=" << backend.name() << '\n';
        backend.doctor(std::cout);
    }
    const auto devices = backend.enumerate();
    if (devices.empty()) {
        std::cout << "No Fastboot USB interfaces found. Check bootloader/fastbootd mode, cable and Windows driver.\n";
        healthy = false;
    }
    for (const auto& device : devices) {
        print_device(device);
        if (!doctor) continue;
        try {
            const auto usb = backend.open(device, config.usb_timeout);
            std::cout << "  access=ok " << usb->details() << '\n';
        } catch (const std::exception& error) {
            healthy = false;
            std::cout << "  access=failed " << error.what() << '\n';
        }
    }
    if (doctor) {
        for (const auto& mapping : config.mappings) {
            std::cout << config.bind << ':' << mapping.port << ' ';
            try {
                const auto listener = Socket::listen(config.bind, mapping.port);
                std::cout << "available\n";
            } catch (const std::exception& error) {
                healthy = false;
                std::cout << "unavailable " << error.what() << '\n';
            }
        }
    }
    return doctor && !healthy ? 2 : 0;
}

int run(const std::vector<std::string>& args) {
    try {
        if (args.empty() || args == std::vector<std::string>{"--help"}) { help(); return 0; }
        if (args == std::vector<std::string>{"--version"}) {
            std::cout << "fastboot-proxy " << PROXY_VERSION << '\n';
            return 0;
        }
        const auto& verb = args.front();
        if (verb != "serve" && verb != "devices" && verb != "doctor") throw Error("unknown command");
        const Network network;
        const auto config = options(args, verb);
        const auto backend = make_backend(config.backend);
        if (verb != "serve") return diagnostics(verb, config, *backend);

#ifdef _WIN32
        if (!SetConsoleCtrlHandler(&console_event, TRUE)) throw Error("cannot register console handler");
#else
        std::signal(SIGINT, signal_handler);
        std::signal(SIGTERM, signal_handler);
#endif
        Logger log(config.level, config.log);
        serve(config, *backend, log, stopped);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "fastboot-proxy: " << error.what() << '\n';
        return 1;
    }
}
}

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        const auto size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[i], -1,
                                               nullptr, 0, nullptr, nullptr);
        if (size <= 0) return 1;
        std::string arg(static_cast<std::size_t>(size), '\0');
        if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[i], -1, arg.data(), size,
                                nullptr, nullptr) != size) return 1;
        arg.pop_back();
        args.push_back(std::move(arg));
    }
    return run(args);
}
#else
int main(int argc, char** argv) {
    return run(std::vector<std::string>(argv + 1, argv + argc));
}
#endif
