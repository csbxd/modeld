#pragma once
#include "modeld/rife.hpp"
#include <atomic>
#include <functional>
#include <nlohmann/json.hpp>
#include <string>
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
using RifeFactory = std::function<std::shared_ptr<Rife>(int, int)>;
struct MediaOptions {
    bool main_stream = false, include_audio = true, include_subtitles = true;
    std::string audio_url;
};
class Video {
    struct Impl;
    std::unique_ptr<Impl> impl;

  public:
    Video(const std::vector<std::string> &sources, const std::string &source_headers,
          int segment_seconds, int qp, const std::string &encoder, RifeFactory factory,
          std::shared_ptr<std::atomic<bool>> cancelled, int source_timeout_seconds,
          MediaOptions options = {});
    ~Video();
    MediaInfo info() const;
    nlohmann::json metadata() const;
    std::vector<uint8_t> header() const;
    Segment render(int64_t sequence);
};
} // namespace modeld
