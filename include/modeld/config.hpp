#pragma once
#include "modeld/core.hpp"
#include <httplib.h>
namespace modeld {
struct ClientConfig {
    std::string endpoint, token_file, connect_address, source_bind = "auto";
    uint64_t cache_bytes = 256ULL << 20;
    int request_timeout_seconds = 45;
    static ClientConfig parse(const Json &value);
    static ClientConfig load(const std::filesystem::path &file = {});
    void configure(httplib::Client &client) const;
};
} // namespace modeld
