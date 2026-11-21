#pragma once
#include "modeld/protocol.hpp"
#include "modeld/rife.hpp"
#include <functional>
namespace modeld {
struct MediaInfo {
    int width, height;
    double duration, source_fps;
};
struct Segment {
    std::vector<uint8_t> bytes;
    int frames = 0;
    double seconds = 0;
};
struct Av1Vbr {
    int target_bps, max_bps;
};
struct RenderSettings {
    int segment_seconds = 2, qp = 18, source_timeout_seconds = 30;
    std::string encoder = "hevc_nvenc";
    bool fragmented_mp4 = false;
    std::optional<Av1Vbr> av1_vbr;
};
using RifeFactory = std::function<std::shared_ptr<Rife>(int, int)>;
class MediaStream {
    struct Impl;
    std::unique_ptr<Impl> impl;

  public:
    MediaStream(Input video, std::optional<Input> audio, TrackSelection tracks,
                RenderSettings settings, RifeFactory factory,
                std::shared_ptr<std::atomic<bool>> canceled);
    ~MediaStream();
    Json description() const;
    std::vector<uint8_t> initialization() const;
    std::string content_type() const;
    Segment render(int64_t index);
};
} // namespace modeld
