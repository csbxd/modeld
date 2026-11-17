#pragma once
#include "modeld/rife.hpp"
#include <atomic>
#include <functional>
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
class Video {
    struct Impl;
    std::unique_ptr<Impl> impl;

  public:
    Video(const std::string &source, int segment_seconds, int qp, const std::string &encoder,
          RifeFactory factory, std::shared_ptr<std::atomic<bool>> cancelled);
    ~Video();
    MediaInfo info() const;
    Segment render(int64_t sequence);
};
} // namespace modeld
