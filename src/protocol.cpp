#include "protocol.hpp"

#include <algorithm>
#include <charconv>

namespace proxy {
Length encode_length(std::uint64_t value) {
    Length bytes{};
    for (auto it = bytes.rbegin(); it != bytes.rend(); ++it) {
        *it = static_cast<std::uint8_t>(value & 0xff);
        value >>= 8;
    }
    return bytes;
}

std::uint64_t decode_length(const Length& input) {
    std::uint64_t size = 0;
    for (const auto octet : input) size = (size << 8) | octet;
    return size;
}

unsigned negotiate(std::string_view hello) {
    const auto decimal = [](char c) { return c >= '0' && c <= '9'; };
    if (hello.size() != 4 || !hello.starts_with("FB") || !decimal(hello[2]) || !decimal(hello[3])) {
        throw Error("invalid Fastboot TCP handshake");
    }
    const auto version = static_cast<unsigned>((hello[2] - '0') * 10 + (hello[3] - '0'));
    if (version == 0) throw Error("unsupported Fastboot TCP version 0");
    return std::min(version, 1U);
}

std::uint32_t data_size(std::string_view value) {
    std::uint32_t result = 0;
    if (value.size() != 8) throw Error("DATA size must contain exactly eight hex digits");
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result, 16);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
        throw Error("invalid DATA size");
    }
    return result;
}

Status status_code(std::string_view message) {
    if (message.size() > status_limit) throw Error("USB status exceeds response limit");
    const auto tag = message.substr(0, 4);
    if (tag == "INFO") return Status::info;
    if (tag == "TEXT") return Status::text;
    if (tag == "OKAY") return Status::okay;
    if (tag == "FAIL") return Status::fail;
    if (tag == "DATA") return Status::data;
    throw Error("invalid USB fastboot status");
}

DataDirection data_direction(std::string_view command) {
    if (command.starts_with("download:")) return DataDirection::download;
    if (command == "upload" || command.starts_with("fetch:")) return DataDirection::upload;
    return DataDirection::unknown;
}

bool changes_mode(std::string_view command) {
    return command == "reboot" || command.starts_with("reboot-") ||
           command == "boot" || command == "continue" || command == "shutdown";
}

namespace {
std::uint64_t read_length(Socket& socket, Millis timeout, const Cancel& stop) {
    Length header{};
    socket.read_exact(header, timeout, stop);
    return decode_length(header);
}

void write_frame(Socket& socket, ConstBytes message, Millis timeout, const Cancel& stop) {
    socket.write_all(encode_length(message.size()), timeout, stop);
    socket.write_all(message, timeout, stop);
}

std::size_t usb_read(Usb& usb, Bytes buffer, const Cancel& stop) {
    for (int empty = 0; empty < 8; ++empty) {
        const auto actual = usb.read(buffer, stop);
        if (actual > buffer.size()) throw Error("invalid USB read length");
        if (actual != 0) return actual;
    }
    throw Error("too many consecutive USB zero-length packets");
}

void usb_write_data(Usb& usb, ConstBytes data, const Cancel& stop) {
    while (!data.empty()) {
        const auto actual = usb.write(data, stop);
        if (actual == data.size()) return;
        if (actual == 0 || actual > data.size() || actual % usb.out_packet_size() != 0) {
            throw Error("USB short write without a safe bulk continuation");
        }
        data = data.subspan(actual);
    }
}

void receive_data(Socket& socket, Usb& usb, Bytes buffer, std::uint64_t amount,
                  Millis timeout, const Cancel& stop, const Cancel& usb_stop) {
    std::uint64_t frame_left = 0;
    while (amount != 0) {
        auto chunk = buffer.first(static_cast<std::size_t>(std::min<std::uint64_t>(amount, buffer.size())));
        const auto filled = chunk;
        while (!chunk.empty()) {
            if (frame_left == 0) {
                frame_left = read_length(socket, timeout, stop);
                if (frame_left == 0 || frame_left > amount) throw Error("invalid download frame length");
            }
            const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(frame_left, chunk.size()));
            socket.read_exact(chunk.first(count), timeout, stop);
            chunk = chunk.subspan(count);
            frame_left -= count;
            amount -= count;
        }
        usb_write_data(usb, filled, usb_stop);
    }
}

void send_data(Socket& socket, Usb& usb, Bytes buffer, std::uint64_t amount,
               Millis timeout, const Cancel& stop, const Cancel& usb_stop) {
    socket.write_all(encode_length(amount), timeout, stop);
    while (amount != 0) {
        const auto chunk = buffer.first(static_cast<std::size_t>(std::min<std::uint64_t>(amount, buffer.size())));
        const auto count = usb_read(usb, chunk, usb_stop);
        socket.write_all(chunk.first(count), timeout, stop);
        amount -= count;
    }
}
}

void bridge(Socket& client, Usb& usb, const Config& config, Logger& log,
            Transaction& transfer, const Cancel& disconnected) {
    std::array<std::uint8_t, chunk_size> buffer{};
    std::array<std::uint8_t, status_limit> reply{};
    const Cancel usb_stop = [&] { return disconnected() || client.peer_closed(); };

    while (!disconnected()) {
        const auto size = read_length(client, config.idle, disconnected);
        if (size == 0 || size > command_limit) throw Error("invalid command frame length");
        const auto raw = Bytes(buffer).first(static_cast<std::size_t>(size));
        client.read_exact(raw, config.io_timeout, disconnected);
        if (!std::ranges::all_of(raw, [](std::uint8_t c) { return c >= 0x20 && c <= 0x7e; })) {
            throw Error("fastboot command contains non-printable ASCII");
        }
        const std::string command(as_text(raw));
        const auto direction = data_direction(command);
        log.write(Level::debug, "command=", log_text(command));
        transfer.phase = Phase::response;
        if (usb.write(raw, usb_stop) != raw.size()) {
            throw Error("partial USB command write; command outcome unknown");
        }

        for (;;) {
            const auto length = usb_read(usb, reply, usb_stop);
            const auto status = ConstBytes(reply).first(length);
            const auto message = as_text(status);
            const auto code = status_code(message);
            log.write(Level::debug, "response=", message.substr(0, 4));
            if (code == Status::okay || code == Status::fail) {
                transfer.phase = Phase::command;
                transfer.transition = code == Status::okay && changes_mode(command);
                // A reboot may detach USB before its final reply can be sent over TCP.
                const Cancel keep_reply = [] { return false; };
                write_frame(client, status, std::min(1000ms, config.io_timeout), keep_reply);
                if (transfer.transition) return;
                break;
            }
            if (code != Status::data) {
                write_frame(client, status, config.io_timeout, disconnected);
                continue;
            }
            if (transfer.phase != Phase::response) throw Error("unexpected additional DATA phase");
            const std::uint64_t amount = data_size(message.substr(4));
            if (amount == 0 || direction == DataDirection::unknown) {
                throw Error("zero-size DATA or unrecognized DATA direction");
            }
            if (direction == DataDirection::download) {
                const auto requested = data_size(std::string_view(command).substr(9));
                if (amount != requested) throw Error("device DATA size differs from download request");
            }
            const auto started = Clock::now();
            write_frame(client, status, config.io_timeout, disconnected);
            if (direction == DataDirection::download) {
                transfer.phase = Phase::download;
                receive_data(client, usb, buffer, amount, config.io_timeout, disconnected, usb_stop);
            } else {
                transfer.phase = Phase::upload;
                send_data(client, usb, buffer, amount, config.io_timeout, disconnected, usb_stop);
            }
            const auto elapsed = std::chrono::duration<double>(Clock::now() - started).count();
            log.write(Level::info, direction == DataDirection::download ? "download" : "upload",
                      " bytes=", amount, " seconds=", elapsed, " MiB/s=",
                      static_cast<double>(amount) / 1048576.0 / std::max(elapsed, 0.000001));
            transfer.phase = Phase::final_response;
        }
    }
}
}
