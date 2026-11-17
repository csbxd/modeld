#pragma once
#include "modeld/limits.hpp"
#include <chrono>
#include <filesystem>
#include <httplib.h>
#include <memory>
#include <nlohmann/json.hpp>
#include <stop_token>
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
void configure_api_client(httplib::Client &client, const Json &config);
Json request_json(const Json &config, const std::string &method, const std::string &path,
                  const Json &body = Json::object(), std::stop_token stop = {});
std::string netbird_address();
std::string media_url(const std::string &url, const Json &config = Json::object());
std::string resolved_video_url(const std::string &opened, const Json &selected_track);
std::string resolved_track_url(const std::string &opened, const Json &selected_track,
                               const std::string &type);
std::vector<std::string> bilibili_video_sources(const std::string &source, const Json &playinfo);
std::vector<std::string> resolve_video_sources(const std::string &webpage,
                                               const std::string &source,
                                               std::stop_token stop = {});
std::filesystem::path client_log_path();
void log_event(const std::string &event, const Json &details = Json::object()) noexcept;
std::string log_url(const std::string &url);
Json log_tracks(const Json &tracks);
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
    struct Cache;
    std::unique_ptr<Impl> impl;
    MediaProxy(const std::string &remote_playlist, double duration,
               const Json &config = Json::object(), std::stop_token stop = {},
               std::shared_ptr<Cache> cache = {});
    ~MediaProxy();
    std::string url() const;
    double buffered_ahead(double position) const;
    std::string failure() const;
    std::shared_ptr<Cache> saved_cache() const;
    MediaProxy(const MediaProxy &) = delete;
};
struct SessionResult {
    Json info;
    std::unique_ptr<MediaProxy> media;
};
struct SessionRequest {
    struct Impl;
    std::unique_ptr<Impl> impl;
    SessionRequest(const Json &config, const std::string &webpage, const std::string &source,
                   const Json &headers, const Json &options = Json::object(),
                   std::shared_ptr<MediaProxy::Cache> cache = {});
    ~SessionRequest();
    bool ready() const;
    std::string stage() const;
    SessionResult take();
    SessionRequest(const SessionRequest &) = delete;
};
} // namespace modeld
