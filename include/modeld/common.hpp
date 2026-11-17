#pragma once
#include <chrono>
#include <filesystem>
#include <httplib.h>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
namespace modeld {
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
inline constexpr auto model_id = "rife-4.26-heavy-fp32";
struct Error : std::runtime_error {
    int status;
    Error(int code, const std::string &text) : std::runtime_error(text), status(code) {}
};
Json read_json(const std::filesystem::path &path);
std::string read_token(const std::filesystem::path &path);
std::string random_id();
std::string sha256_file(const std::filesystem::path &path);
bool secure_equal(const std::string &a, const std::string &b);
Json client_config();
Json request_json(const Json &config, const std::string &method, const std::string &path,
                  const Json &body = Json::object());
std::string netbird_address();
std::string media_url(const std::string &url);
std::string resolved_video_url(const std::string &opened, const Json &selected_track);
Json model_catalog(bool available = true);
std::string playlist(double duration, int fps, int segment_seconds);
struct SourceServer {
    struct Impl;
    std::unique_ptr<Impl> impl;
    SourceServer(const std::filesystem::path &path, const std::string &bind_address);
    ~SourceServer();
    std::string url() const;
    SourceServer(const SourceServer &) = delete;
};
struct MediaProxy {
    struct Impl;
    std::unique_ptr<Impl> impl;
    MediaProxy(const std::string &remote_playlist, double duration);
    ~MediaProxy();
    std::string url() const;
    double buffered_ahead(double position) const;
    MediaProxy(const MediaProxy &) = delete;
};
} // namespace modeld
