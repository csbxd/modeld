#pragma once
#include "modeld/input.hpp"
#include "modeld/stream.hpp"
#include <atomic>
namespace modeld {
class RemoteSession {
    struct Impl;
    std::unique_ptr<Impl> impl;

  public:
    RemoteSession(ApiClient api, const Json &created);
    ~RemoteSession();
    SessionInfo info;
    std::unique_ptr<MediaProxy> stream;
    void paused(bool value);
    RemoteSession(const RemoteSession &) = delete;
};
class OpenSession {
    struct Impl;
    std::unique_ptr<Impl> impl;

  public:
    OpenSession(ClientConfig config, std::string webpage, OpenRequest request,
                std::shared_ptr<MediaProxy::Cache> cache = {});
    ~OpenSession();
    bool ready() const;
    std::string stage() const;
    std::unique_ptr<RemoteSession> take();
    OpenSession(const OpenSession &) = delete;
};
} // namespace modeld
