#include "modeld/common.hpp"
#include <cmath>
#include <iostream>
#include <mpv/client.h>
#include <thread>
namespace {
std::string property(mpv_handle *m, const char *name) {
    char *p = mpv_get_property_string(m, name);
    std::string s = p ? p : "";
    mpv_free(p);
    return s;
}
modeld::Json node_json(const mpv_node &n) {
    using modeld::Json;
    switch (n.format) {
    case MPV_FORMAT_STRING:
        return n.u.string;
    case MPV_FORMAT_FLAG:
        return bool(n.u.flag);
    case MPV_FORMAT_INT64:
        return n.u.int64;
    case MPV_FORMAT_DOUBLE:
        return n.u.double_;
    case MPV_FORMAT_NODE_ARRAY: {
        auto out = Json::array();
        for (int i = 0; i < n.u.list->num; i++)
            out.push_back(node_json(n.u.list->values[i]));
        return out;
    }
    case MPV_FORMAT_NODE_MAP: {
        auto out = Json::object();
        for (int i = 0; i < n.u.list->num; i++)
            out[n.u.list->keys[i]] = node_json(n.u.list->values[i]);
        return out;
    }
    default:
        return nullptr;
    }
}
modeld::Json tracks(mpv_handle *m) {
    mpv_node n{};
    if (mpv_get_property(m, "track-list", MPV_FORMAT_NODE, &n) < 0)
        throw std::runtime_error("Cannot read mpv tracks");
    auto result = node_json(n);
    mpv_free_node_contents(&n);
    return result;
}
void command(mpv_handle *m, std::initializer_list<std::string> values) {
    std::vector<const char *> a;
    for (auto &s : values)
        a.push_back(s.c_str());
    a.push_back(nullptr);
    int r = mpv_command(m, a.data());
    if (r < 0)
        throw std::runtime_error(mpv_error_string(r));
}
void osd(mpv_handle *m, const std::string &s) {
    try {
        command(m, {"show-text", s, "4000"});
    } catch (...) {
    }
}
} // namespace
extern "C" __attribute__((visibility("default"))) int mpv_open_cplugin(mpv_handle *m) {
    using namespace modeld;
    std::string id, original_vid, enabled_entry;
    std::unique_ptr<SourceServer> source;
    Json config;
    auto clear = [&] {
        if (!id.empty()) {
            try {
                request_json(config, "DELETE", "/v1/sessions/" + id);
            } catch (...) {
            }
            id.clear();
        }
        source.reset();
    };
    auto restore = [&] {
        try {
            if (!original_vid.empty() && property(m, "playlist-entry-id") == enabled_entry)
                mpv_set_property_string(m, "vid", original_vid.c_str());
            for (auto &track : tracks(m))
                if (track.value("type", "") == "video" &&
                    track.value("title", "") == "modeld RIFE 4.26 Heavy FP32")
                    command(m, {"video-remove", std::to_string(track.at("id").get<int>())});
        } catch (...) {
        }
        clear();
    };
    try {
        config = client_config();
        std::string name = mpv_client_name(m);
        command(m, {"define-section", "modeld",
                    "Ctrl+Shift+i script-message-to " + name + " toggle\n", "default"});
        command(m, {"enable-section", "modeld", "allow-hide-cursor+allow-vo-dragging"});
        mpv_observe_property(m, 1, "pause", MPV_FORMAT_FLAG);
        auto heartbeat = Clock::now();
        for (;;) {
            auto *e = mpv_wait_event(m, 1.0);
            if (e->event_id == MPV_EVENT_SHUTDOWN)
                break;
            try {
                if (e->event_id == MPV_EVENT_START_FILE) {
                    clear();
                    original_vid.clear();
                    enabled_entry.clear();
                }
                if (e->event_id == MPV_EVENT_END_FILE) {
                    restore();
                    original_vid.clear();
                    enabled_entry.clear();
                }
                if (e->event_id == MPV_EVENT_CLIENT_MESSAGE) {
                    auto *msg = static_cast<mpv_event_client_message *>(e->data);
                    if (msg->num_args < 1 || std::string(msg->args[0]) != "toggle")
                        continue;
                    if (!id.empty()) {
                        restore();
                        osd(m, "RIFE 已关闭，使用原视频轨");
                        continue;
                    }
                    auto path = property(m, "path");
                    if (path.empty())
                        throw Error(422, "请先打开视频");
                    auto entry = property(m, "playlist-entry-id");
                    bool found = false;
                    for (auto &track : tracks(m))
                        if (track.value("type", "") == "video" && track.value("selected", false) &&
                            !track.value("albumart", false))
                            found = true;
                    if (!found)
                        throw Error(422, "当前文件没有可插帧的视频轨");
                    original_vid = property(m, "vid");
                    enabled_entry = entry;
                    osd(m, "正在连接 RIFE 插帧服务…");
                    std::string url;
                    if (path.starts_with("http://") || path.starts_with("https://"))
                        url = path;
                    else {
                        if (path.starts_with("file://"))
                            path.erase(0, 7);
                        auto address = config.value("callback_address", std::string("auto"));
                        source = std::make_unique<SourceServer>(
                            path, address == "auto" ? netbird_address() : address);
                        url = source->url();
                    }
                    auto result = request_json(config, "POST", "/v1/sessions",
                                               {{"model", model_id},
                                                {"task", "frame_interpolation"},
                                                {"source_url", url},
                                                {"target_fps", 60}});
                    id = result.at("id").get<std::string>();
                    if (property(m, "playlist-entry-id") != entry)
                        throw Error(409, "播放文件已变化，请重新开启插帧");
                    command(m,
                            {"video-add", media_url(result.at("playlist_url").get<std::string>()),
                             "select", "modeld RIFE 4.26 Heavy FP32"});
                    osd(m, "RIFE 4.26 Heavy FP32 · 60 fps");
                    heartbeat = Clock::now();
                }
                if (!id.empty() && (Clock::now() - heartbeat > std::chrono::seconds(10) ||
                                    e->event_id == MPV_EVENT_PROPERTY_CHANGE)) {
                    int paused = 0;
                    mpv_get_property(m, "pause", MPV_FORMAT_FLAG, &paused);
                    request_json(config, "POST", "/v1/sessions/" + id + "/heartbeat",
                                 {{"paused", paused != 0}});
                    heartbeat = Clock::now();
                }
            } catch (const std::exception &error) {
                std::cerr << "[modeld] " << error.what() << '\n';
                restore();
                osd(m, std::string("RIFE: ") + error.what());
            }
        }
    } catch (const std::exception &e) {
        std::cerr << "[modeld] " << e.what() << '\n';
        osd(m, std::string("RIFE: ") + e.what());
    }
    clear();
    return 0;
}
