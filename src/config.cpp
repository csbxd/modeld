#include "modeld/config.hpp"
#include <arpa/inet.h>
#include <cstdlib>
namespace modeld {
ClientConfig ClientConfig::parse(const Json &value) {
    require_keys(value, {"endpoint", "token_file", "connect_address", "source_bind", "cache_bytes",
                         "request_timeout_seconds"});
    ClientConfig c;
    c.endpoint = value.at("endpoint");
    c.token_file = value.at("token_file");
    c.connect_address = value.value("connect_address", "");
    c.source_bind = value.value("source_bind", "auto");
    c.cache_bytes = value.value("cache_bytes", uint64_t{256ULL << 20});
    c.request_timeout_seconds = value.value("request_timeout_seconds", 45);
    while (c.endpoint.ends_with('/'))
        c.endpoint.pop_back();
    const auto scheme = c.endpoint.find("://");
    if ((!c.endpoint.starts_with("http://") && !c.endpoint.starts_with("https://")) ||
        c.endpoint.find_first_of("/?#@", scheme + 3) != std::string::npos || c.token_file.empty() ||
        c.cache_bytes < 1 || c.cache_bytes > (2ULL << 30) || c.request_timeout_seconds < 1 ||
        c.request_timeout_seconds > 180)
        throw Error(422, "Invalid client configuration");
    httplib::Client endpoint(c.endpoint);
    if (!endpoint.is_valid())
        throw Error(422, "Invalid API endpoint");
    auto numeric = [](const std::string &text) {
        in6_addr data{};
        return inet_pton(AF_INET, text.c_str(), &data) == 1 ||
               inet_pton(AF_INET6, text.c_str(), &data) == 1;
    };
    if (!c.connect_address.empty() && !numeric(c.connect_address))
        throw Error(422, "connect_address must be a numeric address");
    if (c.source_bind != "auto" && !numeric(c.source_bind))
        throw Error(422, "source_bind must be auto or a numeric address");
    return c;
}
ClientConfig ClientConfig::load(const std::filesystem::path &file) {
    if (!file.empty())
        return parse(read_json(file));
    const char *xdg = std::getenv("XDG_CONFIG_HOME"), *home = std::getenv("HOME");
    const auto user =
        std::filesystem::path(xdg ? xdg : (home ? std::string(home) + "/.config" : "")) /
        "modeld/client.json";
    return parse(read_json(
        std::filesystem::exists(user) ? user : std::filesystem::path("/etc/modeld/client.json")));
}
void ClientConfig::configure(httplib::Client &client) const {
    if (!connect_address.empty())
        client.set_hostname_addr_map({{client.host(), connect_address}});
}
} // namespace modeld
