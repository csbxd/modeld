#include "modeld/common.hpp"
#include <algorithm>
#include <regex>

namespace modeld {
namespace {
struct MediaUrl {
    std::string host, file;
    bool https = false;
};
MediaUrl media_url_parts(const std::string &url) {
    static const std::regex pattern(
        R"(^https?://([^/@?#:]+)(?::[0-9]+)?/[^?#]*/([^/?#]+)(?:[?#].*)?$)");
    std::smatch match;
    if (url.size() > 16384 || !std::regex_match(url, match, pattern))
        return {};
    auto host = match[1].str();
    if (!host.ends_with(".bilivideo.com") && !host.ends_with(".bilivideo.cn"))
        return {};
    return {host, match[2].str(), url.starts_with("https://")};
}
} // namespace

std::vector<std::string> bilibili_video_sources(const std::string &source, const Json &playinfo) {
    const auto chosen = media_url_parts(source);
    if (chosen.file.empty() || !playinfo.is_object() || !playinfo.contains("dash") ||
        !playinfo["dash"].is_object() || !playinfo["dash"].contains("video") ||
        !playinfo["dash"]["video"].is_array())
        return {source};
    std::vector<std::string> urls;
    auto add = [&](const Json &value) {
        if (!value.is_string())
            return;
        const auto url = value.get<std::string>();
        const auto candidate = media_url_parts(url);
        // The complete DASH file name includes cid, part and encoding variant.
        // A lower resolution or a different codec must never become a fallback.
        if (candidate.file == chosen.file &&
            std::find(urls.begin(), urls.end(), url) == urls.end() && urls.size() < 8)
            urls.push_back(url);
    };
    for (const auto &video : playinfo["dash"]["video"]) {
        if (!video.is_object())
            continue;
        for (const auto *name : {"baseUrl", "base_url"})
            if (video.contains(name))
                add(video[name]);
        for (const auto *name : {"backupUrl", "backup_url"})
            if (video.contains(name) && video[name].is_array())
                for (const auto &backup : video[name])
                    add(backup);
    }
    std::stable_sort(urls.begin(), urls.end(), [](const auto &a, const auto &b) {
        auto rank = [](const auto &url) {
            const auto parts = media_url_parts(url);
            return (parts.https ? 0 : 2) + (parts.host.starts_with("upos-") ? 0 : 1);
        };
        return rank(a) < rank(b);
    });
    urls.erase(std::remove(urls.begin(), urls.end(), source), urls.end());
    urls.push_back(source);
    return urls;
}

std::vector<std::string> resolve_video_sources(const std::string &webpage,
                                               const std::string &source) {
    static const std::regex page(
        R"(^https?://(?:www\.)?bilibili\.com/video/(BV[0-9A-Za-z]+)(?:[/?#].*)?$)");
    static const std::regex file(R"(^([0-9]+)-[0-9]+-[0-9]+\.m4s$)");
    std::smatch page_match, file_match;
    const auto media = media_url_parts(source);
    if (!std::regex_match(webpage, page_match, page) ||
        !std::regex_match(media.file, file_match, file))
        return {source};
    try {
        httplib::Client client("https://api.bilibili.com");
        client.set_connection_timeout(3);
        client.set_read_timeout(5);
        client.set_max_timeout(8000);
        client.set_payload_max_length(2 * 1024 * 1024);
        auto response =
            client.Get("/x/player/playurl?bvid=" + page_match[1].str() +
                           "&cid=" + file_match[1].str() + "&fnval=4048&qn=127&fourk=1&try_look=1",
                       httplib::Headers{{"Referer", "https://www.bilibili.com/"},
                                        {"User-Agent", "Mozilla/5.0"}});
        if (!response || response->status != 200)
            throw Error(503, "Bilibili backup address lookup failed");
        auto result = Json::parse(response->body);
        if (result.value("code", -1) != 0 || !result.contains("data"))
            throw Error(503, "Bilibili backup addresses unavailable");
        auto urls = bilibili_video_sources(source, result["data"]);
        log_event("source.alternatives", {{"count", urls.size()},
                                          {"selected", log_url(urls.front())},
                                          {"original", log_url(source)}});
        return urls;
    } catch (const std::exception &e) {
        log_event("source.alternatives.unavailable", {{"error", e.what()}});
        return {source};
    }
}
} // namespace modeld
