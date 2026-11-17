#pragma once
#include "modeld/config.hpp"
#include "modeld/protocol.hpp"
#include <stop_token>
namespace modeld {
class ApiClient {
    ClientConfig config_;

  public:
    explicit ApiClient(ClientConfig config) : config_(std::move(config)) {}
    const ClientConfig &config() const {
        return config_;
    }
    Json request(const std::string &method, const std::string &path, const Json &body = nullptr,
                 std::stop_token stop = {}) const;
    std::unique_ptr<httplib::Client> connection() const;
};
} // namespace modeld
