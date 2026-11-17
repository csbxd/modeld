#include "modeld/common.hpp"
#include <arpa/inet.h>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <ifaddrs.h>
#include <iomanip>
#include <netdb.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <sstream>
namespace modeld {
std::string media_url(const std::string &url, const Json &config) {
    if (!url.starts_with("http://"))
        return url;
    auto slash = url.find('/', 7);
    auto authority = url.substr(7, slash == std::string::npos ? slash : slash - 7);
    auto colon = authority.find(':');
    auto host = authority.substr(0, colon);
    const auto pinned = config.value("api_address", std::string{});
    if (!pinned.empty() && config.contains("api") &&
        httplib::Client(config.at("api").get<std::string>()).host() == host) {
        httplib::Client client(config.at("api").get<std::string>());
        configure_api_client(client, config); // validate the numeric address
        const auto address = pinned.find(':') == std::string::npos ? pinned : "[" + pinned + "]";
        return "http://" + address + authority.substr(host.size()) +
               (slash == std::string::npos ? "" : url.substr(slash));
    }
    if (!host.ends_with(".netbird.cloud"))
        return url;
    // mpv's libcurl can use c-ares, which bypasses systemd-resolved's split DNS.
    // Resolve the private HTTP media host with NSS before handing it to mpv.
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo *addresses = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &addresses) != 0 || !addresses)
        throw Error(503, "Cannot resolve NetBird media host");
    char address[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in *>(addresses->ai_addr)->sin_addr, address,
              sizeof address);
    freeaddrinfo(addresses);
    auto resolved = url;
    resolved.replace(7, host.size(), address);
    return resolved;
}
Json read_json(const std::filesystem::path &p) {
    std::ifstream f(p);
    if (!f)
        throw Error(500, "Cannot read configuration: " + p.string());
    return Json::parse(f);
}
std::string read_token(const std::filesystem::path &p) {
    std::ifstream f(p);
    std::string s;
    std::getline(f, s);
    while (!s.empty() && (s.back() == '\r' || s.back() == ' '))
        s.pop_back();
    if (s.size() < 32)
        throw Error(500, "API token must contain at least 32 characters: " + p.string());
    return s;
}
static std::string hex(const unsigned char *p, size_t n) {
    std::ostringstream s;
    for (size_t i = 0; i < n; i++)
        s << std::hex << std::setfill('0') << std::setw(2) << unsigned(p[i]);
    return s.str();
}
std::string random_id() {
    unsigned char b[16];
    if (RAND_bytes(b, sizeof b) != 1)
        throw Error(500, "Secure random generation failed");
    return hex(b, sizeof b);
}
std::string sha256_file(const std::filesystem::path &path) {
    std::ifstream f(path, std::ios::binary);
    if (!f)
        throw Error(500, "Cannot read model file");
    auto ctx =
        std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr);
    char b[65536];
    while (f) {
        f.read(b, sizeof b);
        EVP_DigestUpdate(ctx.get(), b, f.gcount());
    }
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned n = 0;
    EVP_DigestFinal_ex(ctx.get(), digest, &n);
    return hex(digest, n);
}
bool secure_equal(const std::string &a, const std::string &b) {
    return a.size() == b.size() && CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
}
Json client_config() {
    Json j = {{"api", "http://s8030.netbird.cloud:8765"},
              {"token_file", "/etc/modeld/api.token"},
              {"callback_address", "auto"}};
    if (std::filesystem::exists("/etc/modeld/client.json"))
        j.merge_patch(read_json("/etc/modeld/client.json"));
    const char *x = std::getenv("XDG_CONFIG_HOME");
    const char *h = std::getenv("HOME");
    auto user = std::filesystem::path(x ? x : ((h ? std::string(h) : "") + "/.config")) /
                "modeld/client.json";
    if (std::filesystem::exists(user))
        j.merge_patch(read_json(user));
    return j;
}
void configure_api_client(httplib::Client &client, const Json &config) {
    const auto address = config.value("api_address", std::string{});
    if (address.empty())
        return;
    in6_addr bytes{};
    if (inet_pton(AF_INET, address.c_str(), &bytes) != 1 &&
        inet_pton(AF_INET6, address.c_str(), &bytes) != 1)
        throw Error(422, "api_address must be a numeric IPv4 or IPv6 address");
    httplib::Client endpoint(config.at("api").get<std::string>());
    if (endpoint.host() == client.host())
        // This changes only the connection address; Host, SNI and TLS name checks
        // still use the configured hostname. No system DNS files are modified.
        client.set_hostname_addr_map({{client.host(), address}});
}
Json request_json(const Json &c, const std::string &method, const std::string &path,
                  const Json &body, std::stop_token stop) {
    const auto started = Clock::now();
    const bool heartbeat = path.ends_with("/heartbeat");
    const auto route = path == "/v1/sessions"              ? "session.create"
                       : heartbeat                         ? "session.heartbeat"
                       : path.starts_with("/v1/sessions/") ? "session.close"
                                                           : path;
    if (!heartbeat)
        log_event("api.request", {{"method", method}, {"route", route}});
    httplib::Client client(c.at("api").get<std::string>());
    configure_api_client(client, c);
    const auto wait = path == "/v1/sessions" ? c.value("session_timeout_seconds", 45)
                      : method == "DELETE"   ? 3
                                             : 5;
    if (wait < 1 || wait > 180)
        throw Error(422, "session_timeout_seconds must be between 1 and 180");
    client.set_connection_timeout(method == "DELETE" ? 3 : 5);
    client.set_read_timeout(wait);
    client.set_max_timeout((wait + 5) * 1000);
    client.set_payload_max_length(2 * 1024 * 1024);
    std::stop_callback cancel_request(stop, [&] { client.stop(); });
    if (stop.stop_requested())
        throw Error(499, "Session creation canceled");
    httplib::Headers headers;
    if (path != "/health")
        headers.emplace("Authorization",
                        "Bearer " + read_token(c.at("token_file").get<std::string>()));
    auto result = method == "POST"     ? client.Post(path, headers, body.dump(), "application/json")
                  : method == "DELETE" ? client.Delete(path, headers)
                                       : client.Get(path, headers);
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count();
    if (!result) {
        log_event("api.failed", {{"route", route},
                                 {"elapsed_ms", elapsed},
                                 {"error", httplib::to_string(result.error())}});
        if (stop.stop_requested())
            throw Error(499, "Session creation canceled");
        if (result.error() == httplib::Error::Timeout ||
            (result.error() == httplib::Error::Read && elapsed >= wait * 1000))
            throw Error(504, "Model API response timed out after " + std::to_string(wait) +
                                 " seconds; source probing may be stalled");
        if (result.error() == httplib::Error::Read ||
            result.error() == httplib::Error::ConnectionClosed)
            throw Error(503, "Model API connection closed before a response was received");
        throw Error(503, "Cannot connect to model API " + log_url(c.at("api")) +
                             "; check NetBird routing/DNS and the modeld service (" +
                             httplib::to_string(result.error()) + ")");
    }
    if (!heartbeat || result->status >= 400)
        log_event("api.response", {{"route", route},
                                   {"status", result->status},
                                   {"elapsed_ms", elapsed},
                                   {"bytes", result->body.size()}});
    Json j;
    try {
        j = Json::parse(result->body);
    } catch (...) {
        throw Error(502, "Model API returned a non-JSON response");
    }
    if (result->status >= 400)
        throw Error(result->status, j.value("error", std::string("Model API request failed")));
    return j;
}
std::string netbird_address() {
    ifaddrs *list = nullptr;
    if (getifaddrs(&list) != 0)
        throw Error(500, "Cannot enumerate network interfaces");
    std::string result;
    for (auto *p = list; p; p = p->ifa_next) {
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET)
            continue;
        auto *addr = reinterpret_cast<sockaddr_in *>(p->ifa_addr);
        uint32_t v = ntohl(addr->sin_addr.s_addr);
        if ((v & 0xffc00000u) == 0x64400000u) {
            char s[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &addr->sin_addr, s, sizeof s);
            result = s;
            if (std::string(p->ifa_name) == "wt0")
                break;
        }
    }
    freeifaddrs(list);
    if (result.empty())
        throw Error(503, "NetBird IPv4 address unavailable; configure callback_address");
    return result;
}
Json model_catalog(bool ready) {
    return {{"object", "list"},
            {"data", Json::array({{{"id", model_id},
                                   {"task", "frame_interpolation"},
                                   {"version", "4.26"},
                                   {"variant", "heavy"},
                                   {"precision", "fp32"},
                                   {"tf32", false},
                                   {"backend", "tensorrt-rtx"},
                                   {"state", ready ? "ready" : "loading"},
                                   {"max_width", 1920},
                                   {"max_height", 1920},
                                   {"max_long_side", 1920},
                                   {"max_short_side", 1080},
                                   {"max_pixels", 2073600},
                                   {"target_fps", 60}}})}};
}
std::string playlist(double duration, int fps, int seconds) {
    if (!std::isfinite(duration) || duration <= 0 || duration > 604800 || fps != 60 ||
        seconds < 1 || seconds > 10)
        throw Error(422, "Invalid interpolation timeline");
    auto total = static_cast<int64_t>(std::ceil(duration * fps));
    auto segment = static_cast<int64_t>(fps) * seconds;
    std::ostringstream out;
    out << "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:" << seconds
        << "\n#EXT-X-MEDIA-SEQUENCE:0\n#EXT-X-PLAYLIST-TYPE:VOD\n#EXT-X-INDEPENDENT-SEGMENTS\n";
    for (int64_t n = 0, index = 0; n < total; n += segment, index++) {
        out << "#EXTINF:" << std::fixed << std::setprecision(6)
            << double(std::min(segment, total - n)) / fps << ",\nsegments/" << index << ".ts\n";
    }
    out << "#EXT-X-ENDLIST\n";
    return out.str();
}
} // namespace modeld
