#pragma once

#include <cstdint>
#include <vector>

namespace modeld {
void write_fmp4_recording_duration(std::vector<std::uint8_t> &initialization, double seconds);
}
