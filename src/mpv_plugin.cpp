#include "modeld/common.hpp"
#include <cerrno>
#include <fstream>
#include <iostream>
#include <map>
#include <mpv/client.h>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>

namespace {
using namespace modeld;
std::string property(mpv_handle *m, const char *name) {
    char *value = mpv_get_property_string(m, name);
    std::string result = value ? value : "";
    mpv_free(value);
    return result;
}
Json node_json(const mpv_node &node) {
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
        auto result = Json::array();
        for (int n = 0; n < node.u.list->num; ++n)
            result.push_back(node_json(node.u.list->values[n]));
        return result;
    }
    case MPV_FORMAT_NODE_MAP: {
        auto result = Json::object();
        for (int n = 0; n < node.u.list->num; ++n)
            result[node.u.list->keys[n]] = node_json(node.u.list->values[n]);
        return result;
    }
    default:
        return nullptr;
    }
}
Json node_property(mpv_handle *m, const char *name) {
    mpv_node node{};
    if (mpv_get_property(m, name, MPV_FORMAT_NODE, &node) < 0)
        return nullptr;
    auto result = node_json(node);
    mpv_free_node_contents(&node);
    return result;
}
struct Node {
    mpv_node node{};
    mpv_node_list list{};
    std::string text;
    std::vector<std::unique_ptr<Node>> children;
    std::vector<mpv_node> values;
    std::vector<std::string> names;
    std::vector<char *> keys;
    explicit Node(const Json &value) {
        if (value.is_string()) {
            text = value.get<std::string>();
            node.format = MPV_FORMAT_STRING;
            node.u.string = text.data();
        } else if (value.is_boolean()) {
            node.format = MPV_FORMAT_FLAG;
            node.u.flag = value.get<bool>();
        } else if (value.is_number_integer()) {
            node.format = MPV_FORMAT_INT64;
            node.u.int64 = value.get<int64_t>();
        } else if (value.is_number()) {
            node.format = MPV_FORMAT_DOUBLE;
            node.u.double_ = value.get<double>();
        } else if (value.is_array() || value.is_object()) {
            node.format = value.is_array() ? MPV_FORMAT_NODE_ARRAY : MPV_FORMAT_NODE_MAP;
            for (const auto &[key, item] : value.items()) {
                children.push_back(std::make_unique<Node>(item));
                values.push_back(children.back()->node);
                if (value.is_object())
                    names.push_back(key);
            }
            for (auto &name : names)
                keys.push_back(name.data());
            list.num = values.size();
            list.values = values.data();
            list.keys = keys.data();
            node.u.list = &list;
        }
    }
};
void command(mpv_handle *m, const Json &args, bool async = false) {
    Node node(args);
    int result = async ? mpv_command_node_async(m, 21, &node.node)
                       : mpv_command_node(m, &node.node, nullptr);
    if (result < 0)
        throw Error(503, mpv_error_string(result));
}
void set_node(mpv_handle *m, const char *name, const Json &value) {
    Node node(value);
    const int result = mpv_set_property(m, name, MPV_FORMAT_NODE, &node.node);
    if (result < 0)
        throw Error(503, std::string(name) + ": " + mpv_error_string(result));
}
void osd(mpv_handle *m, const std::string &text, int ms = 4000) {
    try {
        command(m, Json::array({"show-text", text, std::to_string(ms)}));
    } catch (...) {
    }
}
Json tracks(mpv_handle *m) {
    auto result = node_property(m, "track-list");
    return result.is_array() ? result : Json::array();
}
Json selected_track(const Json &all, const std::string &type, const std::string &id = "") {
    for (const auto &track : all)
        if (track.value("type", "") == type && !track.value("albumart", false) &&
            (id.empty() ? track.value("selected", false)
                        : std::to_string(track.value("id", -1)) == id))
            return track;
    return nullptr;
}
struct Original {
    struct Subtitle {
        int fd = -1;
        std::string url;
        ~Subtitle() {
            if (fd >= 0)
                close(fd);
        }
    };
    std::string path, opened, title, vid, aid, sid, secondary_sid, agent, referrer;
    Json tracks, headers, chapters;
    std::map<int, std::shared_ptr<Subtitle>> subtitles;
};
struct Player {
    mpv_handle *m;
    Json config, media_info, load_options;
    Original original;
    std::string id, stage = "startup", cache_identity;
    std::unique_ptr<SourceServer> source, audio_source;
    std::unique_ptr<SessionRequest> pending;
    std::unique_ptr<MediaProxy> media;
    std::shared_ptr<MediaProxy::Cache> cache;
    bool active = false;
    int transition = 0; // 1: processed main; 2: original resolved source; 3: refresh original URL
    Clock::time_point heartbeat = Clock::now();

    explicit Player(mpv_handle *handle) : m(handle), config(client_config()) {}
    ~Player() {
        clear(false);
    }
    double position() const {
        double value = 0;
        mpv_get_property(m, "time-pos", MPV_FORMAT_DOUBLE, &value);
        return std::max(0.0, value);
    }
    void clear(bool keep_cache) {
        pending.reset();
        if (media && keep_cache)
            cache = media->saved_cache();
        if (!keep_cache) {
            cache.reset();
            cache_identity.clear();
        }
        if (!id.empty()) {
            try {
                request_json(config, "DELETE", "/v1/sessions/" + id);
            } catch (...) {
            }
            log_event("session.close");
            id.clear();
        }
        media.reset();
        source.reset();
        audio_source.reset();
        active = false;
    }
    void capture() {
        original.path = property(m, "path");
        original.opened = property(m, "stream-open-filename");
        original.title = property(m, "media-title");
        original.vid = property(m, "vid");
        original.aid = property(m, "aid");
        original.sid = property(m, "sid");
        original.secondary_sid = property(m, "secondary-sid");
        original.tracks = tracks(m);
        // Some subtitle scripts delete their generated ASS on end-file. Keep
        // an anonymous copy across the internal main-media reload; no files
        // are left behind on disk or after this player exits.
        std::map<int, std::shared_ptr<Original::Subtitle>> subtitles;
        size_t subtitle_bytes = 0;
        for (const auto &track : original.tracks) {
            if (track.value("type", "") != "sub" || !track.value("external", false))
                continue;
            auto file = track.value("external-filename", "");
            if (file.starts_with("file://"))
                file.erase(0, 7);
            std::ifstream input(file, std::ios::binary | std::ios::ate);
            if (!input)
                continue;
            const auto size = input.tellg();
            if (size <= 0 || size > 32 * 1024 * 1024 ||
                subtitle_bytes + static_cast<size_t>(size) > 64 * 1024 * 1024)
                continue;
            std::string content(static_cast<size_t>(size), '\0');
            input.seekg(0);
            if (!input.read(content.data(), content.size()))
                continue;
            auto copy = std::make_shared<Original::Subtitle>();
            copy->fd = memfd_create("modeld-subtitle", MFD_CLOEXEC);
            if (copy->fd < 0)
                continue;
            size_t written = 0;
            while (written < content.size()) {
                const auto n = write(copy->fd, content.data() + written, content.size() - written);
                if (n < 0 && errno == EINTR)
                    continue;
                if (n <= 0)
                    break;
                written += n;
            }
            if (written != content.size())
                continue;
            copy->url = "/proc/self/fd/" + std::to_string(copy->fd);
            subtitles[track.at("id").get<int>()] = std::move(copy);
            subtitle_bytes += content.size();
        }
        original.subtitles = std::move(subtitles);
        original.headers = node_property(m, "http-header-fields");
        original.chapters = node_property(m, "chapter-list");
        original.agent = property(m, "user-agent");
        original.referrer = property(m, "referrer");
    }
    std::string source_url(const std::string &opened, const Json &track, const std::string &type,
                           std::unique_ptr<SourceServer> &local) {
        auto url = track.value("external-filename", opened);
        if (url.starts_with("http://") || url.starts_with("https://") || url.starts_with("edl://"))
            return resolved_track_url(url, track, type);
        if (url.starts_with("file://"))
            url.erase(0, 7);
        if (url.empty())
            url = original.path;
        const auto bind = config.value("callback_address", std::string("auto"));
        local = std::make_unique<SourceServer>(url, bind == "auto" ? netbird_address() : bind);
        return local->url();
    }
    void enable() {
        stage = "source.resolve";
        capture();
        if (original.path.empty())
            throw Error(422, "请先打开视频");
        auto video = selected_track(original.tracks, "video");
        if (video.is_null())
            throw Error(422, "当前文件没有可插帧的视频轨");
        const auto width = video.value("demux-w", 0), height = video.value("demux-h", 0);
        if (width > 0 && height > 0 && !supported_dimensions(width, height))
            throw Error(422, "支持偶数尺寸，最长边 1920、最短边 1080");
        auto audio = selected_track(original.tracks, "audio", original.aid);
        const auto video_url = source_url(original.opened, video, "video", source);
        Json options = {{"output_mode", "main"},
                        {"include_audio", !audio.is_null()},
                        {"include_subtitles", true}};
        if (!audio.is_null() &&
            (original.opened.starts_with("edl://") || audio.value("external", false)))
            options["audio_source_url"] = source_url(original.opened, audio, "audio", audio_source);
        // A different DASH audio selection can share identical codec headers.
        // It must not reuse chunks containing the previously selected audio.
        const auto identity =
            original.opened + "\n" + original.aid + "\n" +
            (audio.is_object() ? audio.value("external-filename", std::string{}) : "");
        if (cache_identity != identity)
            cache.reset();
        cache_identity = identity;
        Json headers = Json::object();
        if (original.headers.is_array())
            for (const auto &field : original.headers) {
                const auto value = field.get<std::string>();
                const auto colon = value.find(':');
                if (colon == std::string::npos)
                    continue;
                const auto start = value.find_first_not_of(" \t", colon + 1);
                headers[value.substr(0, colon)] =
                    start == std::string::npos ? "" : value.substr(start);
            }
        if (!original.agent.empty())
            headers["User-Agent"] = original.agent;
        if (!original.referrer.empty())
            headers["Referer"] = original.referrer;
        pending = std::make_unique<SessionRequest>(config, original.path, video_url, headers,
                                                   options, cache);
        osd(m, "正在准备插帧主媒体…再次按 Ctrl+Shift+i 可取消", 60000);
    }
    std::string mapped_track(const std::string &type, const std::string &old_id) const {
        const auto selected = selected_track(original.tracks, type, old_id);
        if (selected.is_null() || (type == "sub" && selected.value("external", false)))
            return "no";
        if (media_info.contains("tracks"))
            for (const auto &track : media_info["tracks"])
                if (track.value("type", "") == type &&
                    ((selected.contains("ff-index") && !track.value("separate", false) &&
                      selected["ff-index"] == track["source_index"]) ||
                     (!selected.contains("ff-index") && selected["id"] == track["id"])))
                    return std::to_string(track.at("id").get<int>());
        // DASH's chosen audio is a separate input, exposed as its first audio track.
        if (type == "audio" && media_info.contains("tracks"))
            for (const auto &track : media_info["tracks"])
                if (track.value("type", "") == "audio")
                    return std::to_string(track.at("id").get<int>());
        return "no";
    }
    void load(int mode) {
        transition = mode;
        const auto at = position();
        Json opts = {{"start", std::to_string(at)},
                     {"resume-playback", "no"},
                     {"force-media-title", original.title},
                     {"vid", mode == 1 ? "1" : original.vid},
                     {"aid", mode == 1 ? mapped_track("audio", original.aid) : original.aid},
                     {"sid", mode == 1 ? mapped_track("sub", original.sid) : original.sid},
                     {"secondary-sid", mode == 1 ? mapped_track("sub", original.secondary_sid)
                                                 : original.secondary_sid}};
        for (const auto *name : {"audio-delay", "sub-delay"}) {
            const auto value = property(m, name);
            if (!value.empty())
                opts[name] = value;
        }
        log_event("main.load.begin", {{"processed", mode == 1}, {"position", at}});
        load_options = std::move(opts);
        // Reload the same entry instead of replacing the whole playlist.
        command(m, Json::array({"playlist-play-index", "current"}), true);
    }
    void hook(const mpv_event_hook &event, bool preloaded) {
        try {
            if (preloaded) {
                if (transition && property(m, "path") == original.path)
                    restore_externals(transition == 1);
                mpv_hook_continue(m, event.id);
                return;
            }
            if (transition && property(m, "path") == original.path) {
                for (const auto &[name, value] : load_options.items()) {
                    const auto option = "file-local-options/" + name;
                    const auto text = value.get<std::string>();
                    if (mpv_set_property_string(m, option.c_str(), text.c_str()) < 0)
                        throw Error(503, "Cannot restore playback option: " + name);
                }
                if (transition != 3) {
                    const auto url = transition == 1 ? media->url() : original.opened;
                    if (mpv_set_property_string(m, "stream-open-filename", url.c_str()) < 0)
                        throw Error(503, "Cannot select the main media stream");
                    mpv_set_property_string(m, "file-local-options/ytdl", "no");
                }
                if (original.headers.is_array())
                    set_node(m, "file-local-options/http-header-fields", original.headers);
                if (!original.agent.empty())
                    mpv_set_property_string(m, "file-local-options/user-agent",
                                            original.agent.c_str());
                if (!original.referrer.empty())
                    mpv_set_property_string(m, "file-local-options/referrer",
                                            original.referrer.c_str());
            }
        } catch (...) {
            mpv_hook_continue(m, event.id);
            throw;
        }
        mpv_hook_continue(m, event.id);
    }
    void restore_externals(bool processed) {
        std::map<std::string, std::string> subtitle_ids;
        for (const auto &track : original.tracks) {
            const auto type = track.value("type", "");
            if (!track.value("external", false) || !track.contains("external-filename") ||
                (type != "sub" && (processed || type != "audio")))
                continue;
            auto filename = track.at("external-filename").get<std::string>();
            const auto old_id = track.at("id").get<int>();
            if (type == "sub" && original.subtitles.contains(old_id))
                filename = original.subtitles.at(old_id)->url;
            bool exists = false;
            for (const auto &loaded : tracks(m))
                exists |= loaded.value("type", "") == type &&
                          loaded.value("external-filename", "") == filename;
            if (!exists)
                command(m, Json::array({type == "sub" ? "sub-add" : "audio-add", filename,
                                        type == "sub"
                                            ? "auto"
                                            : (track.value("selected", false) ? "select" : "auto"),
                                        track.value("title", ""), track.value("lang", "")}));
            if (type == "sub")
                for (const auto &loaded : tracks(m))
                    if (loaded.value("type", "") == "sub" &&
                        loaded.value("external-filename", "") == filename)
                        subtitle_ids[std::to_string(old_id)] =
                            std::to_string(loaded.at("id").get<int>());
        }
        for (const auto &[name, selected] : std::vector<std::pair<std::string, std::string>>{
                 {"sid", original.sid}, {"secondary-sid", original.secondary_sid}}) {
            const auto id = subtitle_ids.contains(selected)
                                ? subtitle_ids.at(selected)
                                : (processed ? mapped_track("sub", selected) : selected);
            mpv_set_property_string(m, name.c_str(), id.c_str());
        }
        if (original.chapters.is_array() && !original.chapters.empty()) {
            try {
                set_node(m, "chapter-list", original.chapters);
            } catch (const std::exception &e) {
                log_event("chapters.restore.failed", {{"error", e.what()}});
            }
        }
    }
    void loaded() {
        if (transition) {
            const bool processed = transition == 1;
            active = processed;
            transition = 0;
            if (!processed)
                clear(true);
            osd(m, processed ? "RIFE 4.26 Heavy FP32 · 60 fps" : "RIFE 已关闭，使用原视频");
            log_event("main.loaded", {{"processed", processed},
                                      {"stream", log_url(property(m, "stream-open-filename"))},
                                      {"tracks", log_tracks(tracks(m))},
                                      {"seekable_cache", property(m, "demuxer-seekable-cache")}});
        }
        log_event("file.loaded",
                  {{"tracks", log_tracks(tracks(m))}, {"format", property(m, "file-format")}});
    }
    void toggle() {
        if (pending) {
            clear(true);
            osd(m, "已取消插帧准备");
            return;
        }
        if (transition) {
            osd(m, "正在切换媒体，请稍候");
            return;
        }
        if (active) {
            load(2);
            return;
        }
        enable();
    }
    void tick(bool pause_changed) {
        if (pending && pending->ready()) {
            stage = pending->stage();
            auto result = pending->take();
            pending.reset();
            id = result.info.at("id").get<std::string>();
            media_info = result.info.at("media");
            media = std::move(result.media);
            cache.reset();
            stage = "main.load";
            load(1);
        }
        if (!id.empty() && (pause_changed || Clock::now() - heartbeat > std::chrono::seconds(10))) {
            request_json(config, "POST", "/v1/sessions/" + id + "/heartbeat",
                         {{"paused", property(m, "pause") == "yes"}});
            heartbeat = Clock::now();
        }
    }
    void failed(const std::string &error) {
        log_event("error", {{"stage", stage}, {"error", error}});
        std::cerr << "[modeld] " << error << '\n';
        if ((active || transition == 1) && !original.path.empty()) {
            try {
                load(2);
            } catch (...) {
                transition = 0;
                clear(true);
            }
        } else {
            transition = 0;
            clear(true);
        }
        osd(m, "RIFE: " + error + "\n详情：modelctl logs", 10000);
    }
};
} // namespace

extern "C" __attribute__((visibility("default"))) int mpv_open_cplugin(mpv_handle *m) {
    try {
        Player player(m);
        mpv_request_log_messages(m, "warn");
        mpv_hook_add(m, 23, "on_load", 5);
        mpv_hook_add(m, 24, "on_preloaded", 50);
        mpv_observe_property(m, 1, "pause", MPV_FORMAT_FLAG);
        log_event("plugin.start", {{"client_version", MODELD_VERSION},
                                   {"mpv_version", property(m, "mpv-version")},
                                   {"api", log_url(player.config.at("api").get<std::string>())}});
        command(m, Json::array({"define-section", "modeld",
                                "Ctrl+Shift+i script-message-to " +
                                    std::string(mpv_client_name(m)) + " toggle\n",
                                "default"}));
        command(m,
                Json::array({"enable-section", "modeld", "allow-hide-cursor+allow-vo-dragging"}));
        for (;;) {
            const auto *event = mpv_wait_event(m, 0.1);
            if (event->event_id == MPV_EVENT_SHUTDOWN)
                break;
            try {
                if (event->event_id == MPV_EVENT_HOOK)
                    player.hook(*static_cast<const mpv_event_hook *>(event->data),
                                event->reply_userdata == 24);
                if (event->event_id == MPV_EVENT_LOG_MESSAGE) {
                    const auto *msg = static_cast<const mpv_event_log_message *>(event->data);
                    log_event("mpv.warning", {{"component", msg->prefix},
                                              {"level", msg->level},
                                              {"message", msg->text}});
                }
                if (event->event_id == MPV_EVENT_START_FILE) {
                    if (!player.transition || property(m, "path") != player.original.path) {
                        player.transition = 0;
                        player.clear(false);
                        player.original = {};
                    }
                    log_event("file.start");
                }
                if (event->event_id == MPV_EVENT_END_FILE) {
                    const auto *end = static_cast<const mpv_event_end_file *>(event->data);
                    log_event("file.end", {{"reason", static_cast<int>(end->reason)},
                                           {"mpv_error", mpv_error_string(end->error)}});
                    if (end->error < 0 && player.transition == 2)
                        player.load(3);
                    else if (end->error < 0 && (player.active || player.transition == 1))
                        player.failed(mpv_error_string(end->error));
                    else if (!player.transition)
                        player.clear(false);
                }
                if (event->event_id == MPV_EVENT_FILE_LOADED)
                    player.loaded();
                if (event->event_id == MPV_EVENT_CLIENT_MESSAGE) {
                    const auto *msg = static_cast<const mpv_event_client_message *>(event->data);
                    if (msg->num_args > 0 && std::string(msg->args[0]) == "toggle")
                        player.toggle();
                }
                if (event->event_id == MPV_EVENT_COMMAND_REPLY && event->error < 0)
                    throw Error(503, mpv_error_string(event->error));
                player.tick(event->event_id == MPV_EVENT_PROPERTY_CHANGE &&
                            event->reply_userdata == 1);
            } catch (const std::exception &error) {
                player.failed(error.what());
            }
        }
        log_event("plugin.stop");
    } catch (const std::exception &error) {
        log_event("fatal", {{"error", error.what()}});
        osd(m, "RIFE: " + std::string(error.what()), 10000);
    }
    return 0;
}
