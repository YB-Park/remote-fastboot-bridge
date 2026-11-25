#pragma once

#include "devices.hpp"
#include "socket.hpp"

#include <array>

namespace proxy {
inline constexpr std::size_t command_limit = 4096;
inline constexpr std::size_t status_limit = 256;
inline constexpr std::size_t chunk_size = 64 * 1024;
inline constexpr std::uint64_t data_limit = 0xffffffff;

using Length = std::array<std::uint8_t, 8>;
Length encode_length(std::uint64_t value);
std::uint64_t decode_length(const Length& input);
unsigned negotiate(std::string_view hello);
std::uint32_t data_size(std::string_view value);

enum class Phase { command, response, download, upload, final_response };
enum class DataDirection { download, upload, unknown };
enum class Status { info, text, okay, fail, data };

Status status_code(std::string_view message);
DataDirection data_direction(std::string_view command);
bool changes_mode(std::string_view command);

struct Transaction {
    Phase phase = Phase::command;
    bool transition = false;
};

void bridge(Socket& client, Usb& usb, const Config& config, Logger& log,
            Transaction& transfer, const Cancel& disconnected);
}
