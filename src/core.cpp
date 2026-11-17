#include "modeld/core.hpp"
#include <algorithm>
#include <arpa/inet.h>
#include <fstream>
#include <ifaddrs.h>
#include <iomanip>
#include <openssl/rand.h>
#include <sstream>
namespace modeld {
Json read_json(const std::filesystem::path &path) {
    std::ifstream file(path);
    if (!file)
        throw Error(500, "Cannot read JSON file: " + path.string());
    return Json::parse(file);
}
std::string read_token(const std::filesystem::path &path) {
    std::ifstream file(path);
    std::string token;
    std::getline(file, token);
    while (!token.empty() && (token.back() == '\r' || token.back() == ' '))
        token.pop_back();
    if (token.size() < 32)
        throw Error(500, "API token must contain at least 32 characters");
    return token;
}
std::string random_id() {
    unsigned char bytes[16];
    if (RAND_bytes(bytes, sizeof bytes) != 1)
        throw Error(500, "Secure random generation failed");
    std::ostringstream result;
    for (auto byte : bytes)
        result << std::hex << std::setfill('0') << std::setw(2) << unsigned(byte);
    return result.str();
}
void require_keys(const Json &value, std::initializer_list<const char *> allowed) {
    if (!value.is_object())
        throw Error(422, "Expected a JSON object");
    for (const auto &[name, item] : value.items())
        if (std::find(allowed.begin(), allowed.end(), name) == allowed.end())
            throw Error(422, "Unknown field: " + name);
}
std::string netbird_address() {
    ifaddrs *list = nullptr;
    if (getifaddrs(&list) != 0)
        throw Error(500, "Cannot enumerate network interfaces");
    struct Guard {
        ifaddrs *list;
        ~Guard() {
            freeifaddrs(list);
        }
    } guard{list};
    std::string result;
    for (auto *p = list; p; p = p->ifa_next) {
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET)
            continue;
        auto *address = reinterpret_cast<sockaddr_in *>(p->ifa_addr);
        if ((ntohl(address->sin_addr.s_addr) & 0xffc00000u) != 0x64400000u)
            continue;
        char text[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &address->sin_addr, text, sizeof text);
        result = text;
        if (std::string(p->ifa_name) == "wt0")
            break;
    }
    if (result.empty())
        throw Error(503, "NetBird address unavailable; set source_bind");
    return result;
}
} // namespace modeld
