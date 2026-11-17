#include "modeld/core.hpp"
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <ctime>
#include <fcntl.h>
#include <fstream>
#include <mutex>
#include <regex>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace modeld {
std::filesystem::path client_log_path() {
    const char *state = std::getenv("XDG_STATE_HOME");
    if (state && std::filesystem::path(state).is_absolute())
        return std::filesystem::path(state) / "modeld/mpv.log";
    const char *home = std::getenv("HOME");
    return home ? std::filesystem::path(home) / ".local/state/modeld/mpv.log"
                : std::filesystem::path{};
}
std::string log_url(const std::string &url) {
    const auto scheme = url.find("://");
    if (scheme == std::string::npos)
        return "<local-file>";
    if (!url.starts_with("http://") && !url.starts_with("https://"))
        return url.substr(0, scheme) + ":<redacted>";
    const auto start = scheme + 3;
    auto end = url.find_first_of("/?#", start);
    if (end == std::string::npos)
        end = url.size();
    auto authority = url.substr(start, end - start);
    const auto at = authority.rfind('@');
    if (at != std::string::npos)
        authority.erase(0, at + 1);
    return url.substr(0, start) + authority + "/[redacted]";
}
namespace {
std::string clean_message(std::string value) {
    size_t position = 0;
    while (position < value.size()) {
        const auto http = value.find("http://", position);
        const auto https = value.find("https://", position);
        const auto start = std::min(http, https);
        if (start == std::string::npos)
            break;
        auto end = value.find_first_of(" \t\r\n\"'<>", start);
        if (end == std::string::npos)
            end = value.size();
        auto safe = log_url(value.substr(start, end - start));
        value.replace(start, end - start, safe);
        position = start + safe.size();
    }
    static const std::regex bearer(R"(Bearer[ \t]+[^ \t\r\n"']+)", std::regex::icase);
    static const std::regex headers(R"((Cookie|Set-Cookie|Authorization)[ \t]*:[^\r\n]*)",
                                    std::regex::icase);
    value = std::regex_replace(value, bearer, "Bearer [redacted]");
    value = std::regex_replace(value, headers, "private-header: [redacted]");
    return value.substr(0, 2048);
}
Json clean(Json value) {
    if (value.is_string())
        return clean_message(value.get<std::string>());
    if (value.is_array())
        for (auto &item : value)
            item = clean(std::move(item));
    if (value.is_object())
        for (auto &[name, item] : value.items()) {
            auto key = name;
            std::transform(key.begin(), key.end(), key.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (key.find("token") != std::string::npos || key.find("cookie") != std::string::npos ||
                key.find("authorization") != std::string::npos || key == "headers")
                item = "<redacted>";
            else
                item = clean(std::move(item));
        }
    return value;
}
struct File {
    int fd = -1;
    ~File() {
        if (fd >= 0)
            ::close(fd);
    }
    bool open(const std::filesystem::path &path, int flags) {
        if (fd >= 0)
            ::close(fd);
        fd = ::open(path.c_str(), flags | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
        struct stat status{};
        return fd >= 0 && ::fstat(fd, &status) == 0 && S_ISREG(status.st_mode) &&
               status.st_uid == ::geteuid() && ::fchmod(fd, 0600) == 0;
    }
};
} // namespace
void log_event(const std::string &event, const Json &details) noexcept {
    try {
        static std::mutex mutex;
        std::lock_guard guard(mutex);
        const auto path = client_log_path();
        if (path.empty())
            return;
        std::filesystem::create_directories(path.parent_path());
        struct stat directory{};
        if (::lstat(path.parent_path().c_str(), &directory) != 0 || !S_ISDIR(directory.st_mode) ||
            directory.st_uid != ::geteuid())
            return;
        ::chmod(path.parent_path().c_str(), 0700);
        File lock, output;
        if (!lock.open(path.parent_path() / "log.lock", O_WRONLY) ||
            ::flock(lock.fd, LOCK_EX) < 0 || !output.open(path, O_WRONLY | O_APPEND))
            return;
        struct stat status{};
        if (::fstat(output.fd, &status) == 0 && status.st_size > 2 * 1024 * 1024) {
            std::filesystem::rename(path, path.string() + ".1");
            if (!output.open(path, O_WRONLY | O_APPEND))
                return;
        }
        const auto now = std::chrono::system_clock::now();
        const auto time = std::chrono::system_clock::to_time_t(now);
        std::tm utc{};
        gmtime_r(&time, &utc);
        char stamp[32];
        std::strftime(stamp, sizeof stamp, "%Y-%m-%dT%H:%M:%SZ", &utc);
        static const std::string process = [] {
            std::ifstream input("/proc/self/comm");
            std::string name;
            std::getline(input, name);
            return name;
        }();
        auto line = Json({{"time", stamp},
                          {"pid", ::getpid()},
                          {"process", process},
                          {"client_version", MODELD_VERSION},
                          {"event", event},
                          {"details", clean(details)}})
                        .dump(-1, ' ', false, Json::error_handler_t::replace) +
                    "\n";
        if (line.size() > 16384)
            return;
        size_t offset = 0;
        while (offset < line.size()) {
            auto count = ::write(output.fd, line.data() + offset, line.size() - offset);
            if (count < 0 && errno == EINTR)
                continue;
            if (count <= 0)
                break;
            offset += static_cast<size_t>(count);
        }
    } catch (...) {
    }
}
Json log_tracks(const Json &tracks) {
    Json result = Json::array();
    if (tracks.is_array())
        for (const auto &track : tracks) {
            Json summary = Json::object();
            for (const auto *key : {"id", "type", "selected", "external", "codec", "decoder",
                                    "demux-w", "demux-h", "demux-fps", "program-id"})
                if (track.contains(key))
                    summary[key] = track.at(key);
            result.push_back(std::move(summary));
        }
    return result;
}
} // namespace modeld
