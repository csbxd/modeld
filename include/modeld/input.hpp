#pragma once
#include "modeld/protocol.hpp"
#include <memory>
#include <stop_token>
namespace modeld {
std::string resolved_track_url(const std::string &opened, const Json &track,
                               const std::string &kind);
std::vector<std::string> bilibili_sources(const std::string &source, const Json &playinfo);
std::vector<std::string> resolve_sources(const std::string &webpage, const std::string &source,
                                         std::stop_token stop = {});
struct SourceServer {
    struct Impl;
    std::unique_ptr<Impl> impl;
    SourceServer(const std::filesystem::path &file, const std::string &bind_address);
    ~SourceServer();
    std::string url() const;
    SourceServer(const SourceServer &) = delete;
};
} // namespace modeld
