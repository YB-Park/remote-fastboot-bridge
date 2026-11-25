#include "devices.hpp"
#include "protocol.hpp"

#include <algorithm>
#include <charconv>
#include <deque>
#include <iomanip>

namespace proxy {
namespace {
struct VirtualDevice {
    VirtualDevice(std::string serial_number, std::string usb_location)
        : serial(std::move(serial_number)), location(std::move(usb_location)) {}
    std::string serial;
    std::string location;
    std::atomic_uint64_t generation{1};
    Clock::time_point visible_at{};
    bool userspace = false;
    std::uint64_t download_hash = 0;
    std::uint64_t download_size = 0;
};

constexpr std::string_view payload{"OKAYFAILINFODATA00000000TEXT\x00\xff", 30};
constexpr std::uint64_t hash_seed = 14695981039346656037ULL;
constexpr std::uint64_t hash_prime = 1099511628211ULL;

std::string hex(std::uint64_t value, unsigned width) {
    std::ostringstream out;
    out << std::hex << std::setfill('0') << std::setw(static_cast<int>(width)) << value;
    return out.str();
}

class MockUsb final : public Usb {
public:
    MockUsb(VirtualDevice& device, std::mutex& state, Millis timeout)
        : device_(device), state_(state), timeout_(timeout), generation_(device.generation.load()) {}

    bool removed() const override { return generation_ != device_.generation.load(); }
    std::size_t out_packet_size() const override { return 512; }
    std::string details() const override { return "mock ff/42/03 IN=0x81 OUT=0x02 packet=512"; }

    std::size_t write(ConstBytes data, const Cancel& stop) override {
        if (stop()) throw Error("mock write cancelled");
        if (download_left_ != 0) {
            auto count = std::min<std::uint64_t>(data.size(), download_left_);
            if (count > 16384) count = 16384;
            for (const auto ch : data.first(static_cast<std::size_t>(count))) {
                download_hash_ = (download_hash_ ^ ch) * hash_prime;
            }
            download_left_ -= count;
            if (download_left_ == 0) {
                device_.download_hash = download_hash_;
                device_.download_size = original_download_;
                responses_.push_back("OKAY");
            }
            return static_cast<std::size_t>(count);
        }
        command(as_text(data));
        return data.size();
    }

    std::size_t read(Bytes data, const Cancel& stop) override {
        if (stop()) throw Error("mock read cancelled");
        if (!responses_.empty()) {
            const auto reply = std::move(responses_.front());
            responses_.pop_front();
            if (reply.size() > data.size()) throw Error("mock response does not fit read");
            std::ranges::copy(bytes(reply), data.begin());
            if (reboot_ && reply.starts_with("OKAY")) detach(*reboot_);
            return reply.size();
        }
        if (upload_left_ != 0) {
            const auto count = static_cast<std::size_t>(std::min<std::uint64_t>({
                data.size(), upload_left_, 33001}));
            for (std::size_t i = 0; i < count; ++i) {
                const auto pos = static_cast<std::size_t>((upload_offset_ + i) % payload.size());
                data[i] = static_cast<std::uint8_t>(payload[pos]);
            }
            upload_offset_ += count;
            upload_left_ -= count;
            if (upload_left_ == 0) responses_.push_back("OKAY");
            return count;
        }
        const auto until = Clock::now() + timeout_;
        while (!stop() && Clock::now() < until) std::this_thread::sleep_for(10ms);
        throw Error("mock USB read timeout or cancelled");
    }

private:
    void detach(bool userspace) {
        std::scoped_lock lock(state_);
        device_.userspace = userspace;
        device_.visible_at = Clock::now() + 450ms;
        device_.download_size = 0;
        device_.download_hash = 0;
        ++device_.generation;
    }

    void upload(std::uint64_t size, std::uint64_t offset = 0) {
        if (size == 0 || size > data_limit) { responses_.push_back("FAILinvalid fetch size"); return; }
        upload_left_ = size;
        upload_offset_ = offset;
        responses_.push_back("DATA" + hex(size, 8));
    }

    void command(std::string_view cmd) {
        if (cmd == "getvar:product") responses_.push_back("OKAYproxy-mock");
        else if (cmd == "getvar:serialno") responses_.push_back("OKAY" + device_.serial);
        else if (cmd == "getvar:is-userspace") responses_.push_back(device_.userspace ? "OKAYyes" : "OKAYno");
        else if (cmd == "getvar:version-bootloader") responses_.push_back("OKAYmock-bl");
        else if (cmd == "getvar:version-baseband") responses_.push_back("OKAYmock-radio");
        else if (cmd == "getvar:slot-count") responses_.push_back("OKAY0");
        else if (cmd == "getvar:current-slot") responses_.push_back("OKAY");
        else if (cmd == "getvar:snapshot-update-status") responses_.push_back("OKAYnone");
        else if (cmd == "getvar:is-logical:mock") responses_.push_back("OKAYno");
        else if (cmd == "getvar:all") {
            responses_ = {"", "INFOproduct: proxy-mock", "INFOserialno: " + device_.serial,
                          std::string("TEXTmock-text\0", 14), "OKAY"};
        } else if (cmd == "getvar:max-download-size") responses_.push_back("OKAY0xffffffff");
        else if (cmd == "getvar:max-fetch-size") responses_.push_back("OKAY0x00400000");
        else if (cmd == "getvar:partition-size:mock") responses_.push_back("OKAY0x0050007f");
        else if (cmd == "getvar:has-slot:mock") responses_.push_back("OKAYno");
        else if (cmd == "getvar:mock-download-hash") responses_.push_back("OKAY" + hex(device_.download_hash, 16));
        else if (cmd == "getvar:mock-download-size") responses_.push_back("OKAY" + hex(device_.download_size, 8));
        else if (cmd.starts_with("download:")) {
            const auto size = data_size(cmd.substr(9));
            if (size == 0) { responses_.push_back("FAILempty download"); return; }
            original_download_ = download_left_ = size;
            download_hash_ = hash_seed;
            responses_.push_back("DATA" + hex(size, 8));
        } else if (cmd == "upload") {
            upload(5 * 1024 * 1024 + 127);
        } else if (cmd.starts_with("fetch:mock:")) {
            auto arguments = cmd.substr(11);
            const auto divide = arguments.find(':');
            const auto parse_hex = [](std::string_view numbers) -> std::uint64_t {
                std::uint64_t value{};
                if (numbers.starts_with("0x")) numbers.remove_prefix(2);
                const auto parsed = std::from_chars(numbers.data(), numbers.data() + numbers.size(), value, 16);
                if (parsed.ec != std::errc{} || parsed.ptr != numbers.data() + numbers.size()) {
                    throw Error("invalid mock fetch argument");
                }
                return value;
            };
            if (divide == arguments.npos) { responses_.push_back("FAILmissing fetch size"); return; }
            upload(parse_hex(arguments.substr(divide + 1)), parse_hex(arguments.substr(0, divide)));
        } else if (cmd == "flash:mock" && device_.userspace && device_.download_size > 0) {
            responses_.push_back("OKAYmock flash");
        } else if (cmd == "oem mock-malformed") responses_.push_back("DATA1234567g");
        else if (cmd == "oem mock-unknown-data") responses_.push_back("DATA00000001");
        else if (cmd == "oem mock-timeout") return;
        else if (cmd == "oem mock-detach") detach(device_.userspace);
        else if (changes_mode(cmd)) {
            reboot_ = cmd == "reboot-fastboot";
            responses_.push_back("OKAYrebooting");
        } else responses_.push_back("FAILunsupported mock command");
    }

    VirtualDevice& device_;
    std::mutex& state_;
    Millis timeout_;
    std::uint64_t generation_;
    std::deque<std::string> responses_;
    std::optional<bool> reboot_;
    std::uint64_t original_download_ = 0;
    std::uint64_t download_left_ = 0;
    std::uint64_t download_hash_ = 0;
    std::uint64_t upload_left_ = 0;
    std::uint64_t upload_offset_ = 0;
};

class MockBackend final : public Backend {
public:
    std::string name() const override { return "mock"; }
    void doctor(std::ostream& out) const override { out << "Simulated USB inventory (development build)\n"; }

    std::vector<Device> enumerate() override {
        std::scoped_lock lock(state_);
        std::vector<Device> result;
        for (const auto& unit : hardware_) {
            if (Clock::now() < unit.visible_at) continue;
            result.push_back({unit.serial, instance(unit), unit.location, "MOCK", 0, 0, L"mock"});
        }
        return result;
    }

    std::unique_ptr<Usb> open(const Device& device, Millis timeout) override {
        std::scoped_lock lock(state_);
        for (auto& unit : hardware_) {
            if (device.instance == instance(unit) && Clock::now() >= unit.visible_at) {
                return std::make_unique<MockUsb>(unit, state_, timeout);
            }
        }
        throw Error("mock device unavailable");
    }

private:
    static std::string instance(const VirtualDevice& unit) {
        return unit.location + (unit.userspace ? "-fastbootd" : "-bootloader");
    }
    std::mutex state_;
    VirtualDevice hardware_[5]{
        {"MOCK_A", "mock-a"}, {"MOCK_B", "mock-b"}, {"DUPLICATE", "mock-duplicate-1"},
        {"DUPLICATE", "mock-duplicate-2"}, {"", "mock-no-serial"}
    };
};
}

std::unique_ptr<Backend> make_mock() { return std::make_unique<MockBackend>(); }
}
