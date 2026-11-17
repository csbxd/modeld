#pragma once
#include "modeld/api.hpp"
namespace modeld {
class MediaProxy {
    struct Impl;
    std::unique_ptr<Impl> impl;

  public:
    struct Cache;
    MediaProxy(const ApiClient &api, Manifest manifest, std::stop_token stop = {},
               std::shared_ptr<Cache> cache = {});
    ~MediaProxy();
    std::string url() const;
    std::shared_ptr<Cache> saved_cache() const;
    MediaProxy(const MediaProxy &) = delete;
};
} // namespace modeld
