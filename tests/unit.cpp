#include "protocol.hpp"

#include <limits>

namespace {
using namespace proxy;

void check(bool condition, std::string_view label) {
    if (!condition) throw Error("unit test failed: " + std::string(label));
}

template<class Callback>
void reject(Callback callback, std::string_view label) {
    try {
        callback();
    } catch (const Error&) {
        return;
    }
    check(false, label);
}

void framing() {
    const Length expected{0x01, 0x02, 0x03, 0x04, 0x12, 0x34, 0x56, 0x78};
    check(encode_length(0x0102030412345678) == expected, "big endian encode");
    check(decode_length(expected) == 0x0102030412345678, "big endian decode");
    check(decode_length(encode_length(std::numeric_limits<std::uint64_t>::max())) ==
          std::numeric_limits<std::uint64_t>::max(), "uint64 maximum");
    check(negotiate("FB01") == 1 && negotiate("FB99") == 1, "version negotiation");
    for (const auto hello : {"FB00", "FBa1", "AB01", "FB1", "FB+1", "FB011"}) {
        reject([&] { negotiate(hello); }, hello);
    }
}

void protocol() {
    check(data_size("fF00ae01") == 0xff00ae01, "hex mixed case");
    check(data_size("ffffffff") == 0xffffffff, "uint32 maximum");
    for (const auto size : {"0000000", "fffffffff", "0000000g", "0x000001", "+0000001"}) {
        reject([&] { data_size(size); }, size);
    }
    const std::string text("TEXTone\0three", 13);
    check(status_code(text) == Status::text, "TEXT contains NUL");
    check(status_code("INFOhello") == Status::info, "INFO");
    check(status_code("DATA12345678") == Status::data, "DATA");
    check(status_code("FAIL") == Status::fail && status_code("OKAY") == Status::okay, "terminal responses");
    reject([] { status_code("OKA"); }, "truncated status");
    reject([] { status_code("OKAY" + std::string(253, 'a')); }, "status limit");
    check(data_direction("download:00000001") == DataDirection::download, "download");
    check(data_direction("fetch:boot:0:4") == DataDirection::upload, "fetch");
    check(data_direction("upload") == DataDirection::upload, "upload");
    check(data_direction("oem vendor") == DataDirection::unknown, "opaque OEM");
    check(changes_mode("reboot-fastboot") && changes_mode("boot") &&
          !changes_mode("getvar:boot"), "mode transition");
}

void mapping() {
    Device a{"serial", "instanceA", "locationA", "MOCK", 0, 0, L"mock"};
    Device b{"serial", "instanceB", "locationB", "MOCK", 0, 0, L"mock"};
    Device c{"serial-alias", "changed", "locationA", "MOCK", 0, 0, L"mock"};
    Selector selector{"serial", {}, {}, {}};
    reject([&] { unique_device({a, b}, selector, {}); }, "duplicate serial rejected");
    check(unique_device({a, b}, selector, "locationA")->instance == "instanceA", "location pin");
    check(!unique_device({b}, selector, "locationA"), "no physical fallback");
    selector.location = "LOCATIONa";
    selector.aliases = {"serial-alias"};
    check(unique_device({c}, selector, {})->instance == "changed", "explicit serial alias");
    check(device_key(a) == device_key(c) && device_key(a) != device_key(b), "physical lock identity");
    const Network network;
    check(subnet("127.0.0.1/8").contains(ipv4("127.255.0.1")), "CIDR allowed");
    check(!subnet("127.0.0.1").contains(ipv4("127.0.0.2")), "CIDR denied");
    reject([] { subnet("127.0.0.1/33"); }, "CIDR invalid");
    Config config;
    config.mappings = {{selector, 5554}, {selector, 5554}};
    reject([&] { validate(config); }, "duplicate port");
}
}

int main() {
    try {
        framing();
        protocol();
        mapping();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
