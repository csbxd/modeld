#include "modeld/mpv_state.hpp"
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <fstream>
#include <sys/mman.h>
#include <unistd.h>
namespace modeld {
namespace {
Json json(const mpv_node &node) {
    switch (node.format) {
    case MPV_FORMAT_STRING:
        return node.u.string;
    case MPV_FORMAT_FLAG:
        return bool(node.u.flag);
    case MPV_FORMAT_INT64:
        return node.u.int64;
    case MPV_FORMAT_DOUBLE:
        return node.u.double_;
    case MPV_FORMAT_NODE_ARRAY: {
        auto value = Json::array();
        for (int i = 0; i < node.u.list->num; ++i)
            value.push_back(json(node.u.list->values[i]));
        return value;
    }
    case MPV_FORMAT_NODE_MAP: {
        auto value = Json::object();
        for (int i = 0; i < node.u.list->num; ++i)
            value[node.u.list->keys[i]] = json(node.u.list->values[i]);
        return value;
    }
    default:
        return nullptr;
    }
}
struct Node {
    mpv_node value{};
    mpv_node_list list{};
    std::string text;
    std::vector<std::unique_ptr<Node>> children;
    std::vector<mpv_node> values;
    std::vector<std::string> names;
    std::vector<char *> keys;
    explicit Node(const Json &input) {
        if (input.is_string()) {
            text = input.get<std::string>();
            value.format = MPV_FORMAT_STRING;
            value.u.string = text.data();
        } else if (input.is_boolean()) {
            value.format = MPV_FORMAT_FLAG;
            value.u.flag = input.get<bool>();
        } else if (input.is_number_integer()) {
            value.format = MPV_FORMAT_INT64;
            value.u.int64 = input.get<int64_t>();
        } else if (input.is_number()) {
            value.format = MPV_FORMAT_DOUBLE;
            value.u.double_ = input.get<double>();
        } else if (input.is_array() || input.is_object()) {
            value.format = input.is_array() ? MPV_FORMAT_NODE_ARRAY : MPV_FORMAT_NODE_MAP;
            for (const auto &[key, item] : input.items()) {
                children.push_back(std::make_unique<Node>(item));
                values.push_back(children.back()->value);
                if (input.is_object())
                    names.push_back(key);
            }
            for (auto &name : names)
                keys.push_back(name.data());
            list.num = static_cast<int>(values.size());
            list.values = values.data();
            list.keys = keys.data();
            value.u.list = &list;
        }
    }
};
std::string selected(const Json &tracks, const std::string &kind, int channel = 0) {
    for (const auto &track : tracks)
        if (track.value("type", "") == kind && track.value("selected", false) &&
            track.at("main-selection") == channel)
            return std::to_string(track.at("id").get<int>());
    return "no";
}
} // namespace
std::string Mpv::text(const char *name) const {
    char *value = mpv_get_property_string(handle_, name);
    std::string result = value ? value : "";
    mpv_free(value);
    return result;
}
Json Mpv::get(const char *name) const {
    mpv_node value{};
    if (mpv_get_property(handle_, name, MPV_FORMAT_NODE, &value) < 0)
        return nullptr;
    auto result = json(value);
    mpv_free_node_contents(&value);
    return result;
}
void Mpv::set(const std::string &name, const Json &value) const {
    Node node(value);
    const auto status = mpv_set_property(handle_, name.c_str(), MPV_FORMAT_NODE, &node.value);
    if (status < 0)
        throw Error(503, name + ": " + mpv_error_string(status), "player_failed");
}
void Mpv::command(const Json &args, bool async) const {
    Node node(args);
    const auto status = async ? mpv_command_node_async(handle_, 21, &node.value)
                              : mpv_command_node(handle_, &node.value, nullptr);
    if (status < 0)
        throw Error(503, mpv_error_string(status), "player_failed");
}
void Mpv::osd(const std::string &message, int milliseconds) const {
    try {
        command(Json::array({"show-text", message, std::to_string(milliseconds)}));
    } catch (...) {
    }
}
double Mpv::position() const {
    const auto value = get("time-pos");
    return value.is_number() ? std::max(0., value.get<double>()) : 0.;
}
bool Mpv::paused() const {
    return get("pause") == true;
}
Playback::Subtitle::~Subtitle() {
    if (fd >= 0)
        ::close(fd);
}
Playback Playback::capture(const Mpv &mpv) {
    Playback p;
    p.path = mpv.text("path");
    p.opened = mpv.text("stream-open-filename");
    p.webpage = p.path;
    p.title = mpv.text("media-title");
    p.edition = mpv.text("edition");
    p.agent = mpv.text("user-agent");
    p.referrer = mpv.text("referrer");
    p.tracks = mpv.get("track-list");
    p.headers = mpv.get("http-header-fields");
    p.chapters = mpv.get("chapter-list");
    if (!p.tracks.is_array())
        throw Error(422, "Current file has no tracks");
    p.video = selected(p.tracks, "video");
    p.audio = selected(p.tracks, "audio");
    p.subtitle = selected(p.tracks, "sub");
    p.secondary_subtitle = selected(p.tracks, "sub", 1);
    const auto ytdl = mpv.get("user-data/mpv/ytdl/json-subprocess-result");
    if (ytdl.is_object() && ytdl.contains("stdout") && ytdl["stdout"].is_string()) {
        const auto data = Json::parse(ytdl["stdout"].get<std::string>(), nullptr, false);
        if (data.is_object() && data.contains("webpage_url") && data["webpage_url"].is_string())
            p.webpage = data["webpage_url"];
    }
    size_t total = 0;
    for (const auto &track : p.tracks) {
        if (track.value("type", "") != "sub" || !track.value("external", false))
            continue;
        auto file = track.value("external-filename", "");
        if (file.starts_with("file://"))
            file.erase(0, 7);
        std::ifstream input(file, std::ios::binary | std::ios::ate);
        if (!input)
            continue;
        const auto size = input.tellg();
        if (size <= 0 || size > (32 << 20) || total + static_cast<size_t>(size) > (64ULL << 20))
            continue;
        std::string content(static_cast<size_t>(size), '\0');
        input.seekg(0);
        if (!input.read(content.data(), content.size()))
            continue;
        auto snapshot = std::make_shared<Subtitle>();
        snapshot->fd = memfd_create("modeld-subtitle", MFD_CLOEXEC);
        if (snapshot->fd < 0)
            continue;
        size_t written = 0;
        while (written < content.size()) {
            const auto n = write(snapshot->fd, content.data() + written, content.size() - written);
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0)
                break;
            written += n;
        }
        if (written != content.size())
            continue;
        snapshot->url = "/proc/self/fd/" + std::to_string(snapshot->fd);
        p.subtitles[track.at("id").get<int>()] = std::move(snapshot);
        total += content.size();
    }
    return p;
}
Json Playback::track(const std::string &kind, const std::string &id) const {
    for (const auto &track : tracks)
        if (track.value("type", "") == kind && std::to_string(track.at("id").get<int>()) == id)
            return track;
    return nullptr;
}
Json Playback::request_headers() const {
    auto output = Json::object();
    if (headers.is_array())
        for (const auto &header : headers) {
            const auto text = header.get<std::string>();
            const auto colon = text.find(':');
            if (colon == std::string::npos)
                continue;
            auto name = text.substr(0, colon);
            std::transform(name.begin(), name.end(), name.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            const auto begin = text.find_first_not_of(" \t", colon + 1);
            output[name] = begin == std::string::npos ? "" : text.substr(begin);
        }
    if (!agent.empty())
        output["user-agent"] = agent;
    if (!referrer.empty())
        output["referer"] = referrer;
    return output;
}
std::string Playback::mapped(const Manifest &manifest, const std::string &kind,
                             const std::string &id) const {
    const auto original = track(kind, id);
    if (original.is_null() || (kind == "sub" && original.value("external", false)))
        return "no";
    const auto type = kind == "sub" ? "subtitle" : kind;
    // A selected DASH audio URL is its own input. Its single output track has
    // no relationship to ff-index on mpv's aggregate EDL demuxer.
    if (kind == "audio") {
        const Json *separate = nullptr;
        int count = 0;
        for (const auto &output : manifest.media.at("tracks"))
            if (output.at("kind") == "audio" && output.at("source") == "audio") {
                separate = &output;
                ++count;
            }
        if (count == 1)
            return std::to_string(separate->at("id").get<int>());
    }
    if (!original.contains("ff-index"))
        throw Error(422, "Selected track has no source stream index", "unsupported_track_mapping");
    for (const auto &output : manifest.media.at("tracks"))
        if (output.at("kind") == type && output.at("source_index") == original.at("ff-index") &&
            (kind == "audio" || output.at("source") == "video"))
            return std::to_string(output.at("id").get<int>());
    return "no";
}
void Playback::restore(const Mpv &mpv, const Manifest *processed) const {
    std::map<std::string, std::string> ids;
    for (const auto &track : tracks) {
        const auto kind = track.value("type", "");
        if (!track.value("external", false) || !track.contains("external-filename") ||
            (kind != "sub" && (processed || kind != "audio")))
            continue;
        auto filename = track.at("external-filename").get<std::string>();
        const auto id = track.at("id").get<int>();
        if (kind == "sub" && subtitles.contains(id))
            filename = subtitles.at(id)->url;
        bool exists = false;
        for (const auto &loaded : mpv.get("track-list"))
            exists |= loaded.value("type", "") == kind &&
                      loaded.value("external-filename", "") == filename;
        if (!exists)
            mpv.command(Json::array(
                {kind == "sub" ? "sub-add" : "audio-add", filename,
                 kind == "sub" ? "auto" : (track.value("selected", false) ? "select" : "auto"),
                 track.value("title", ""), track.value("lang", "")}));
        if (kind == "sub")
            for (const auto &loaded : mpv.get("track-list"))
                if (loaded.value("type", "") == "sub" &&
                    loaded.value("external-filename", "") == filename)
                    ids[std::to_string(id)] = std::to_string(loaded.at("id").get<int>());
    }
    for (const auto &[name, old] : std::vector<std::pair<std::string, std::string>>{
             {"sid", subtitle}, {"secondary-sid", secondary_subtitle}}) {
        const auto id = ids.contains(old) ? ids.at(old)
                        : processed       ? mapped(*processed, "sub", old)
                                          : old;
        mpv.set(name, id);
    }
    if (chapters.is_array() && !chapters.empty())
        mpv.set("chapter-list", chapters);
}
} // namespace modeld
