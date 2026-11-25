#pragma once

#include "devices.hpp"

namespace proxy {
void serve(const Config& config, Backend& backend, Logger& log, const std::atomic_bool& stop);
}
