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
modeld::Json node_property(mpv_handle *m, const char *name) {
    mpv_node n{};
    if (mpv_get_property(m, name, MPV_FORMAT_NODE, &n) < 0)
        return nullptr;
    auto value = node_json(n);
    mpv_free_node_contents(&n);
    return value;
}
std::string playing_entry(mpv_handle *m) {
    auto position = property(m, "playlist-playing-pos");
    return position.empty() ? "" : property(m, ("playlist/" + position + "/id").c_str());
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
void osd(mpv_handle *m, const std::string &s, int milliseconds = 4000) {
    try {
        command(m, {"show-text", s, std::to_string(milliseconds)});
    } catch (...) {
    }
}
} // namespace
extern "C" __attribute__((visibility("default"))) int mpv_open_cplugin(mpv_handle *m) {
    using namespace modeld;
    std::string id, original_vid, enabled_entry, original_cache_seek;
    std::unique_ptr<SourceServer> source;
    std::unique_ptr<MediaProxy> media;
    std::unique_ptr<SessionRequest> pending;
    bool buffering = false, activating = false;
    int activation_pause = 0;
    double media_duration = 0;
    std::string stage = "startup";
    Json config;
    auto clear = [&] {
        if (pending) {
            log_event("session.create.cancel");
            pending.reset();
        }
        if (!id.empty())
            log_event("session.close");
        if (!id.empty()) {
            try {
                request_json(config, "DELETE", "/v1/sessions/" + id);
            } catch (...) {
            }
            id.clear();
        }
        media.reset();
        source.reset();
        if (!original_cache_seek.empty()) {
            if (playing_entry(m) == enabled_entry)
                mpv_set_property_string(m, "file-local-options/demuxer-seekable-cache",
                                        original_cache_seek.c_str());
            original_cache_seek.clear();
        }
        if (buffering || activating) {
            int pause = activating ? activation_pause : 0;
            mpv_set_property(m, "pause", MPV_FORMAT_FLAG, &pause);
        }
        buffering = activating = false;
    };
    auto restore = [&] {
        try {
            if (!original_vid.empty() && playing_entry(m) == enabled_entry)
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
        mpv_request_log_messages(m, "warn");
        log_event("plugin.start", {{"client_version", MODELD_VERSION},
                                   {"mpv_version", property(m, "mpv-version")},
                                   {"api", log_url(config.at("api").get<std::string>())}});
        std::string name = mpv_client_name(m);
        command(m, {"define-section", "modeld",
                    "Ctrl+Shift+i script-message-to " + name + " toggle\n", "default"});
        command(m, {"enable-section", "modeld", "allow-hide-cursor+allow-vo-dragging"});
        mpv_observe_property(m, 1, "pause", MPV_FORMAT_FLAG);
        mpv_observe_property(m, 2, "seeking", MPV_FORMAT_FLAG);
        mpv_observe_property(m, 3, "track-list", MPV_FORMAT_NODE);
        auto heartbeat = Clock::now();
        for (;;) {
            auto *e = mpv_wait_event(m, 0.25);
            if (e->event_id == MPV_EVENT_SHUTDOWN)
                break;
            try {
                if (e->event_id == MPV_EVENT_LOG_MESSAGE) {
                    const auto *message = static_cast<mpv_event_log_message *>(e->data);
                    log_event("mpv.warning", {{"component", message->prefix},
                                              {"level", message->level},
                                              {"message", message->text}});
                }
                if (e->event_id == MPV_EVENT_PROPERTY_CHANGE && e->reply_userdata == 3) {
                    const auto *changed = static_cast<mpv_event_property *>(e->data);
                    if (changed->format == MPV_FORMAT_NODE && changed->data)
                        log_event("tracks.changed",
                                  {{"tracks", log_tracks(node_json(*static_cast<const mpv_node *>(
                                                  changed->data)))}});
                }
                if (e->event_id == MPV_EVENT_START_FILE) {
                    log_event("file.start");
                    clear();
                    original_vid.clear();
                    enabled_entry.clear();
                }
                if (e->event_id == MPV_EVENT_END_FILE) {
                    const auto *end = static_cast<mpv_event_end_file *>(e->data);
                    log_event("file.end", {{"reason", static_cast<int>(end->reason)},
                                           {"mpv_error", mpv_error_string(end->error)}});
                    restore();
                    original_vid.clear();
                    enabled_entry.clear();
                }
                if (e->event_id == MPV_EVENT_FILE_LOADED)
                    log_event("file.loaded", {{"tracks", log_tracks(tracks(m))},
                                              {"vo", property(m, "current-vo")},
                                              {"format", property(m, "file-format")}});
                auto *msg = e->event_id == MPV_EVENT_CLIENT_MESSAGE
                                ? static_cast<mpv_event_client_message *>(e->data)
                                : nullptr;
                // Other scripts can broadcast continuously. Their messages must
                // still pass through buffering and heartbeat maintenance.
                if (msg && msg->num_args > 0 && std::string(msg->args[0]) == "toggle") {
                    stage = "toggle";
                    log_event("toggle",
                              {{"active", !id.empty()}, {"tracks", log_tracks(tracks(m))}});
                    if (!id.empty() || pending) {
                        restore();
                        osd(m, "RIFE 已关闭，使用原视频轨");
                        continue;
                    }
                    auto path = property(m, "path");
                    if (path.empty())
                        throw Error(422, "请先打开视频");
                    auto entry = playing_entry(m);
                    Json selected;
                    for (auto &track : tracks(m))
                        if (track.value("type", "") == "video" && track.value("selected", false) &&
                            !track.value("albumart", false))
                            selected = track;
                    if (selected.is_null())
                        throw Error(422, "当前文件没有可插帧的视频轨");
                    const auto width = selected.value("demux-w", 0);
                    const auto height = selected.value("demux-h", 0);
                    if (width > 0 && height > 0 && !supported_dimensions(width, height))
                        throw Error(422, "支持偶数尺寸，最长边 1920、最短边 1080；当前 " +
                                             std::to_string(width) + "×" + std::to_string(height));
                    original_vid = property(m, "vid");
                    enabled_entry = entry;
                    mpv_get_property(m, "pause", MPV_FORMAT_FLAG, &activation_pause);
                    activating = true;
                    int pause = 1;
                    mpv_set_property(m, "pause", MPV_FORMAT_FLAG, &pause);
                    osd(m, "正在准备 RIFE 插帧…再次按 Ctrl+Shift+i 可取消", 60000);
                    std::string url;
                    Json headers = Json::object();
                    auto opened = property(m, "stream-open-filename");
                    auto external = selected.value("external-filename", std::string{});
                    if (!external.empty())
                        opened = external;
                    stage = "source.resolve";
                    log_event("source.resolve",
                              {{"opened", log_url(opened)},
                               {"timeline_bytes", opened.size()},
                               {"selected", log_tracks(Json::array({selected}))}});
                    if (opened.starts_with("http://") || opened.starts_with("https://") ||
                        opened.starts_with("edl://")) {
                        url = resolved_video_url(opened, selected);
                        auto fields = node_property(m, "http-header-fields");
                        if (fields.is_array())
                            for (const auto &field : fields) {
                                auto value = field.get<std::string>();
                                auto colon = value.find(':');
                                if (colon != std::string::npos) {
                                    auto start = value.find_first_not_of(" \t", colon + 1);
                                    headers[value.substr(0, colon)] =
                                        start == std::string::npos ? "" : value.substr(start);
                                }
                            }
                        auto agent = property(m, "user-agent");
                        auto referrer = property(m, "referrer");
                        if (!agent.empty())
                            headers["User-Agent"] = agent;
                        if (!referrer.empty())
                            headers["Referer"] = referrer;
                    } else {
                        if (path.starts_with("file://"))
                            path.erase(0, 7);
                        auto address = config.value("callback_address", std::string("auto"));
                        source = std::make_unique<SourceServer>(
                            path, address == "auto" ? netbird_address() : address);
                        url = source->url();
                    }
                    stage = "session.create";
                    pending = std::make_unique<SessionRequest>(config, path, url, headers);
                }
                if (pending && pending->ready()) {
                    stage = pending->stage();
                    auto result = pending->take();
                    id = result.info.at("id").get<std::string>();
                    pending.reset();
                    if (playing_entry(m) != enabled_entry)
                        throw Error(409, "播放文件已变化，请重新开启插帧");
                    media_duration = result.info.at("duration").get<double>();
                    stage = "media.timeline";
                    media = std::move(result.media);
                    // Cached seeks can bypass the proxy after its generation
                    // changed, leaving the buffer guard waiting for segments
                    // mpv will never request. Keep forward buffering, but make
                    // seeks go through the HLS source while RIFE is enabled.
                    original_cache_seek = property(m, "file-local-options/demuxer-seekable-cache");
                    if (mpv_set_property_string(m, "file-local-options/demuxer-seekable-cache",
                                                "no") < 0)
                        throw Error(503, "Cannot configure the interpolated video seek cache");
                    log_event("playback.cache",
                              {{"seekable_cache", false}, {"previous", original_cache_seek}});
                    stage = "video.add";
                    double activation_position = 0;
                    mpv_get_property(m, "time-pos", MPV_FORMAT_DOUBLE, &activation_position);
                    log_event("video.add.begin");
                    command(m,
                            {"video-add", media->url(), "select", "modeld RIFE 4.26 Heavy FP32"});
                    log_event("video.add.ready", {{"tracks", log_tracks(tracks(m))}});
                    // video-add may read from zero while discarding frames to
                    // catch up. Explicitly seek all tracks so enabling RIFE in
                    // a long video requests the current HLS segment directly.
                    stage = "video.align";
                    command(m, {"seek", std::to_string(activation_position), "absolute+exact"});
                    log_event("video.aligned", {{"position", activation_position}});
                    activating = false;
                    mpv_set_property(m, "pause", MPV_FORMAT_FLAG, &activation_pause);
                    osd(m, "RIFE 4.26 Heavy FP32 · 60 fps");
                    heartbeat = Clock::now();
                    stage = "playback";
                }
                bool buffer_changed = false;
                int paused = 0, seeking = 0;
                mpv_get_property(m, "pause", MPV_FORMAT_FLAG, &paused);
                mpv_get_property(m, "seeking", MPV_FORMAT_FLAG, &seeking);
                if (!id.empty() && media) {
                    if (auto error = media->failure(); !error.empty())
                        throw Error(503, error);
                    double position = 0;
                    if (mpv_get_property(m, "time-pos", MPV_FORMAT_DOUBLE, &position) >= 0) {
                        auto ahead = media->buffered_ahead(position);
                        const auto remaining = media_duration - position;
                        // HLS probing and the HEVC decoder can retain more than
                        // one segment before releasing frames. Downloaded bytes
                        // alone therefore need a larger safety margin.
                        if (!paused && !seeking && !buffering &&
                            ahead < std::min(6.0, remaining) - 0.05 && remaining > 1.0) {
                            buffering = true;
                            paused = 1;
                            buffer_changed = true;
                            mpv_set_property(m, "pause", MPV_FORMAT_FLAG, &paused);
                            osd(m, "远端视频缓冲中…");
                            log_event("buffer.wait", {{"position", position}, {"ahead", ahead}});
                        } else if (buffering && ahead >= std::min(8.0, remaining) - 0.05) {
                            buffering = false;
                            paused = 0;
                            buffer_changed = true;
                            mpv_set_property(m, "pause", MPV_FORMAT_FLAG, &paused);
                            log_event("buffer.resume", {{"position", position}, {"ahead", ahead}});
                        }
                    }
                }
                if (!id.empty() &&
                    (buffer_changed || Clock::now() - heartbeat > std::chrono::seconds(10) ||
                     e->event_id == MPV_EVENT_PROPERTY_CHANGE)) {
                    mpv_get_property(m, "pause", MPV_FORMAT_FLAG, &paused);
                    request_json(config, "POST", "/v1/sessions/" + id + "/heartbeat",
                                 {{"paused", paused != 0 && !buffering && seeking == 0}});
                    heartbeat = Clock::now();
                }
            } catch (const std::exception &error) {
                log_event("error", {{"stage", stage}, {"error", error.what()}});
                std::cerr << "[modeld] " << error.what() << '\n';
                restore();
                osd(m, std::string("RIFE: ") + error.what() + "\n详情：modelctl logs", 10000);
            }
        }
    } catch (const std::exception &e) {
        log_event("fatal", {{"stage", stage}, {"error", e.what()}});
        std::cerr << "[modeld] " << e.what() << '\n';
        osd(m, std::string("RIFE: ") + e.what() + "\n详情：modelctl logs", 10000);
    }
    clear();
    log_event("plugin.stop");
    return 0;
}
