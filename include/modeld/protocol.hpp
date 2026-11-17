#pragma once
#include "modeld/core.hpp"
#include "modeld/limits.hpp"
#include <optional>
#include <vector>
namespace modeld {
inline constexpr const char *api_prefix = "/api/v2";
inline constexpr const char *model_id = "rife-4.26-heavy-fp32";
struct Input {
    std::vector<std::string> urls;
    Json headers = Json::object();
    static Input parse(const Json &value);
    Json json() const;
    std::string http_headers() const;
};
struct TrackSelection {
    bool audio = true, subtitles = true;
    Json json() const;
};
struct OpenRequest {
    Input video;
    std::optional<Input> audio;
    TrackSelection tracks;
    Json json() const;
};
struct Manifest {
    Json media;
    std::string init, segment_template;
    double duration = 0;
    int segment_seconds = 0;
    int64_t segment_count = 0;
    static Manifest parse(const Json &value);
    std::string segment(int64_t index) const;
    std::string edl(const std::string &local_base) const;
};
struct SessionInfo {
    std::string id;
    int lease_seconds = 0;
    Manifest manifest;
    static SessionInfo parse(const Json &value);
};
} // namespace modeld
