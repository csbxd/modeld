#include "modeld/protocol.hpp"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
namespace modeld {
Input Input::parse(const Json &value) {
    require_keys(value, {"urls", "headers"});
    Input input{value.at("urls").get<std::vector<std::string>>(),
                value.value("headers", Json::object())};
    if (input.urls.empty() || input.urls.size() > 9 || !input.headers.is_object())
        throw Error(422, "Input requires one to nine HTTP(S) URLs and an object of headers");
    for (const auto &url : input.urls)
        if ((!url.starts_with("http://") && !url.starts_with("https://")) ||
            url.find_first_of("\r\n") != std::string::npos)
            throw Error(422, "Input URLs must use HTTP(S)");
    for (const auto &[name, value] : input.headers.items())
        if (name.find_first_of("\r\n") != std::string::npos || !value.is_string() ||
            value.get_ref<const std::string &>().find_first_of("\r\n") != std::string::npos)
            throw Error(422, "Invalid input header");
    return input;
}
Json Input::json() const {
    return {{"urls", urls}, {"headers", headers}};
}
std::string Input::http_headers() const {
    std::string result;
    for (const auto &[name, value] : headers.items())
        result += name + ": " + value.get<std::string>() + "\r\n";
    return result;
}
Json TrackSelection::json() const {
    return {{"audio", audio}, {"subtitles", subtitles}};
}
Json OpenRequest::json() const {
    return {{"model", model_id},
            {"video", video.json()},
            {"audio", audio ? audio->json() : Json(nullptr)},
            {"tracks", tracks.json()}};
}
Manifest Manifest::parse(const Json &value) {
    require_keys(value, {"schema", "media", "init", "segment_template"});
    if (value.at("schema") != 2)
        throw Error(502, "Unsupported media schema");
    Manifest m;
    m.media = value.at("media");
    require_keys(m.media, {"width", "height", "duration_seconds", "source_fps", "output_fps",
                           "segment_seconds", "segment_count", "tracks"});
    m.duration = m.media.at("duration_seconds");
    m.segment_seconds = m.media.at("segment_seconds");
    m.segment_count = m.media.at("segment_count");
    m.init = value.at("init");
    m.segment_template = value.at("segment_template");
    const auto suffix = std::string("/media/init");
    if (!m.init.starts_with("/api/v2/sessions/") || !m.init.ends_with(suffix) ||
        m.init.find_first_of("?#%") != std::string::npos ||
        m.segment_template != m.init.substr(0, m.init.size() - 4) + "segments/{index}" ||
        !std::isfinite(m.duration) || m.duration <= 0 || m.duration > 604800 ||
        m.segment_seconds < 1 || m.segment_seconds > 10 || m.media.at("output_fps") != 60 ||
        m.segment_count != static_cast<int64_t>(std::ceil(std::ceil(m.duration * 60) /
                                                          (m.segment_seconds * 60.0))) ||
        !m.media.at("tracks").is_array())
        throw Error(502, "Invalid media manifest");
    return m;
}
std::string Manifest::segment(int64_t index) const {
    if (index < 0 || index >= segment_count)
        throw Error(404, "Segment does not exist");
    auto path = segment_template;
    path.replace(path.find("{index}"), 7, std::to_string(index));
    return path;
}
std::string Manifest::edl(const std::string &base) const {
    auto quote = [](const std::string &value) {
        return "%" + std::to_string(value.size()) + "%" + value;
    };
    std::ostringstream out;
    out << "# mpv EDL v0\n!no_chapters\n!mp4_dash,init=" << quote(base + "/init") << '\n';
    out << std::fixed << std::setprecision(9);
    for (int64_t i = 0; i < segment_count; ++i) {
        const double begin = i * static_cast<double>(segment_seconds);
        const double end = std::min(std::ceil(duration * 60) / 60, begin + segment_seconds);
        out << quote(base + "/segments/" + std::to_string(i)) << ",length=" << end - begin << '\n';
    }
    return out.str();
}
SessionInfo SessionInfo::parse(const Json &value) {
    require_keys(value, {"id", "lease_seconds", "manifest"});
    SessionInfo info{value.at("id"), value.at("lease_seconds"),
                     Manifest::parse(value.at("manifest"))};
    if (info.id.size() != 32 ||
        info.id.find_first_not_of("0123456789abcdef") != std::string::npos ||
        info.lease_seconds < 30 ||
        info.manifest.init != std::string(api_prefix) + "/sessions/" + info.id + "/media/init")
        throw Error(502, "Invalid session response");
    return info;
}
} // namespace modeld
