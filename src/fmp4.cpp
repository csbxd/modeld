#include "modeld/fmp4.hpp"
#include "modeld/core.hpp"
#include <array>
#include <cmath>
#include <limits>
#include <optional>
#include <span>
#include <string_view>

namespace modeld {
namespace {
constexpr size_t box_header_size = 8;
using Bytes = std::span<const uint8_t>;

uint32_t read_u32(Bytes bytes, size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < 4)
        throw Error(502, "Truncated MP4 initialization box");
    const auto *p = bytes.data() + offset;
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

void write_integer(std::span<uint8_t> bytes, uint64_t value) {
    for (auto i = bytes.rbegin(); i != bytes.rend(); ++i) {
        *i = static_cast<uint8_t>(value);
        value >>= 8;
    }
}

struct Box {
    size_t offset;
    size_t size;

    Bytes payload(Bytes parent) const {
        return parent.subspan(offset + box_header_size, size - box_header_size);
    }
};

std::optional<Box> find_box(Bytes boxes, std::string_view name) {
    for (size_t offset = 0; offset < boxes.size();) {
        const size_t length = read_u32(boxes, offset);
        if (length < box_header_size || length > boxes.size() - offset)
            throw Error(502, "Invalid MP4 initialization box size");
        const auto *type = reinterpret_cast<const char *>(boxes.data() + offset + 4);
        if (std::string_view(type, 4) == name)
            return Box{offset, length};
        offset += length;
    }
    return std::nullopt;
}
} // namespace

void write_fmp4_recording_duration(std::vector<uint8_t> &initialization, double seconds) {
    const auto moov = find_box(initialization, "moov");
    if (!moov)
        throw Error(502, "Missing MP4 movie box");
    const auto movie = moov->payload(initialization);
    const auto mvhd = find_box(movie, "mvhd");
    const auto mvex = find_box(movie, "mvex");
    if (!mvhd || !mvex)
        throw Error(502, "Missing fragmented MP4 movie headers");

    const auto header = mvhd->payload(movie);
    if (header.empty() || header.front() > 1)
        throw Error(502, "Invalid MP4 movie header version");
    const uint32_t timescale = read_u32(header, header.front() == 0 ? 12 : 20);
    const long double ticks = std::ceil(static_cast<long double>(seconds) * timescale);
    if (!std::isfinite(seconds) || ticks < 1 || ticks >= std::ldexp(1.0L, 64))
        throw Error(502, "Invalid MP4 recording duration");

    // A version 1 Movie Extends Header preserves the full recording duration
    // even when it exceeds 32 bits at the muxer's movie timescale.
    std::array<uint8_t, 20> mehd{0, 0, 0, 20, 'm', 'e', 'h', 'd', 1};
    write_integer(std::span(mehd).subspan(12), static_cast<uint64_t>(ticks));
    const auto prior = find_box(mvex->payload(movie), "mehd");
    const size_t old_size = prior ? prior->size : 0;
    if (prior && old_size != 16 && old_size != mehd.size())
        throw Error(502, "Invalid MP4 movie extends header");
    const size_t growth = mehd.size() - old_size;
    if (moov->size > std::numeric_limits<uint32_t>::max() - growth)
        throw Error(502, "MP4 movie box is too large");
    const size_t mvex_offset = moov->offset + box_header_size + mvex->offset;
    const size_t offset = mvex_offset + box_header_size + (prior ? prior->offset : 0);
    initialization.reserve(initialization.size() + growth);
    const auto insertion = initialization.erase(initialization.begin() + offset,
                                                initialization.begin() + offset + old_size);
    initialization.insert(insertion, mehd.begin(), mehd.end());
    auto bytes = std::span(initialization);
    write_integer(bytes.subspan(moov->offset, 4), moov->size + growth);
    write_integer(bytes.subspan(mvex_offset, 4), mvex->size + growth);
}
} // namespace modeld
