#include "modeld/mpv_state.hpp"
#include "modeld/session.hpp"
#include <iostream>
namespace {
using namespace modeld;
enum class Phase { Original, Opening, LoadingProcessed, Processed, Restoring, Refreshing };
const char *name(Phase phase) {
    switch (phase) {
    case Phase::Original:
        return "original";
    case Phase::Opening:
        return "opening";
    case Phase::LoadingProcessed:
        return "loading_processed";
    case Phase::Processed:
        return "processed";
    case Phase::Restoring:
        return "restoring";
    case Phase::Refreshing:
        return "refreshing";
    }
    return "invalid";
}
struct Player {
    Mpv mpv;
    ClientConfig config = ClientConfig::load();
    Playback original;
    Phase phase = Phase::Original;
    std::unique_ptr<OpenSession> opening;
    std::unique_ptr<RemoteSession> session;
    std::unique_ptr<SourceServer> video_source, audio_source;
    std::shared_ptr<MediaProxy::Cache> cache;
    std::string cache_key;
    Json load_options;
    explicit Player(mpv_handle *handle) : mpv(handle) {}
    bool loading() const {
        return phase == Phase::LoadingProcessed || phase == Phase::Restoring ||
               phase == Phase::Refreshing;
    }
    void clear(bool retain) {
        opening.reset();
        if (retain && session && session->stream)
            cache = session->stream->saved_cache();
        if (!retain) {
            cache.reset();
            cache_key.clear();
        }
        session.reset();
        video_source.reset();
        audio_source.reset();
        phase = Phase::Original;
    }
    ~Player() {
        clear(false);
    }
    Input input(const Json &track, const std::string &kind, std::unique_ptr<SourceServer> &shared) {
        auto url = track.value("external-filename", original.opened);
        if (url.starts_with("http://") || url.starts_with("https://") || url.starts_with("edl://"))
            return {{resolved_track_url(url, track, kind)}, original.request_headers()};
        if (url.starts_with("file://"))
            url.erase(0, 7);
        if (url.empty())
            url = original.path;
        shared = std::make_unique<SourceServer>(
            url, config.source_bind == "auto" ? netbird_address() : config.source_bind);
        return {{shared->url()}, Json::object()};
    }
    void enable() {
        auto snapshot = Playback::capture(mpv);
        if (snapshot.path == original.path && snapshot.webpage == snapshot.path)
            snapshot.webpage = original.webpage;
        original = std::move(snapshot);
        const auto video = original.track("video", original.video);
        if (original.path.empty() || video.is_null() || video.value("albumart", false))
            throw Error(422, "请先打开可插帧的视频");
        const int width = video.value("demux-w", 0), height = video.value("demux-h", 0);
        if (width > 0 && height > 0 && !supported_dimensions(width, height))
            throw Error(422, "支持偶数尺寸，最长边 1920、最短边 1080");
        OpenRequest request;
        request.video = input(video, "video", video_source);
        const auto audio = original.track("audio", original.audio);
        request.tracks.audio = !audio.is_null();
        if (!audio.is_null() &&
            (original.opened.starts_with("edl://") || audio.value("external", false)))
            request.audio = input(audio, "audio", audio_source);
        const auto identity = (video_source ? original.opened : request.video.urls.front()) + "\n" +
                              original.video + "\n" + original.audio + "\n" +
                              (request.audio ? (audio_source ? audio.value("external-filename", "")
                                                             : request.audio->urls.front())
                                             : "embedded");
        if (identity != cache_key)
            cache.reset();
        cache_key = identity;
        opening =
            std::make_unique<OpenSession>(config, original.webpage, std::move(request), cache);
        phase = Phase::Opening;
        mpv.osd("正在准备 RIFE 主媒体…再次按 Ctrl+Shift+i 可取消", 60000);
    }
    void load(Phase target) {
        const bool processed = target == Phase::LoadingProcessed;
        load_options = {
            {"start", std::to_string(mpv.position())},
            {"resume-playback", "no"},
            {"force-media-title", original.title},
            {"vid", processed ? "1" : original.video},
            {"aid", processed ? original.mapped(session->info.manifest, "audio", original.audio)
                              : original.audio},
            {"sid", processed ? original.mapped(session->info.manifest, "sub", original.subtitle)
                              : original.subtitle},
            {"secondary-sid",
             processed ? original.mapped(session->info.manifest, "sub", original.secondary_subtitle)
                       : original.secondary_subtitle}};
        for (const auto *option : {"audio-delay", "sub-delay"}) {
            const auto value = mpv.text(option);
            if (!value.empty())
                load_options[option] = value;
        }
        if (!original.edition.empty())
            load_options["edition"] = processed ? "auto" : original.edition;
        phase = target;
        log_event("player.reload", {{"phase", name(phase)}, {"position", mpv.position()}});
        mpv.command(Json::array({"playlist-play-index", "current"}), true);
    }
    void hook(bool preloaded) {
        if (!loading() || mpv.text("path") != original.path)
            return;
        if (preloaded) {
            original.restore(mpv,
                             phase == Phase::LoadingProcessed ? &session->info.manifest : nullptr);
            return;
        }
        for (const auto &[key, value] : load_options.items())
            mpv.set("file-local-options/" + key, value);
        if (phase != Phase::Refreshing) {
            mpv.set("stream-open-filename",
                    phase == Phase::LoadingProcessed ? session->stream->url() : original.opened);
            mpv.set("file-local-options/ytdl", "no");
        }
        if (original.headers.is_array())
            mpv.set("file-local-options/http-header-fields", original.headers);
        if (!original.agent.empty())
            mpv.set("file-local-options/user-agent", original.agent);
        if (!original.referrer.empty())
            mpv.set("file-local-options/referrer", original.referrer);
    }
    void loaded() {
        if (loading()) {
            if (phase == Phase::LoadingProcessed) {
                phase = Phase::Processed;
                session->paused(mpv.paused());
                mpv.osd("RIFE 4.26 Heavy FP32 · 60 fps");
            } else {
                clear(true);
                mpv.osd("RIFE 已关闭，使用原视频");
            }
        }
        log_event("player.loaded", {{"phase", name(phase)},
                                    {"format", mpv.text("file-format")},
                                    {"tracks", log_tracks(mpv.get("track-list"))}});
    }
    void toggle() {
        if (phase == Phase::Opening) {
            clear(true);
            mpv.osd("已取消插帧准备");
        } else if (loading())
            mpv.osd("正在切换媒体，请稍候");
        else if (phase == Phase::Processed)
            load(Phase::Restoring);
        else
            enable();
    }
    void tick() {
        if (opening && opening->ready()) {
            session = opening->take();
            opening.reset();
            cache.reset();
            session->paused(mpv.paused());
            load(Phase::LoadingProcessed);
        }
    }
    void failed(const std::exception &error) {
        log_event("player.failed", {{"phase", name(phase)},
                                    {"stage", opening ? opening->stage() : "playback"},
                                    {"error", error.what()}});
        std::cerr << "[modeld] " << error.what() << '\n';
        if ((phase == Phase::Processed || phase == Phase::LoadingProcessed) &&
            !original.path.empty()) {
            try {
                load(Phase::Restoring);
            } catch (...) {
                clear(true);
            }
        } else
            clear(true);
        mpv.osd("RIFE: " + std::string(error.what()) + "\n详情：modelctl logs", 10000);
    }
};
} // namespace
extern "C" __attribute__((visibility("default"))) int mpv_open_cplugin(mpv_handle *handle) {
    using namespace modeld;
    try {
        Player player(handle);
        mpv_request_log_messages(handle, "warn");
        mpv_hook_add(handle, 23, "on_load", 5);
        mpv_hook_add(handle, 24, "on_preloaded", 50);
        mpv_observe_property(handle, 1, "pause", MPV_FORMAT_FLAG);
        log_event("plugin.start", {{"version", MODELD_VERSION},
                                   {"protocol", 2},
                                   {"mpv", player.mpv.text("mpv-version")},
                                   {"endpoint", log_url(player.config.endpoint)}});
        player.mpv.command(Json::array(
            {"define-section", "modeld",
             "Ctrl+Shift+i script-message-to " + std::string(mpv_client_name(handle)) + " toggle\n",
             "default"}));
        player.mpv.command(
            Json::array({"enable-section", "modeld", "allow-hide-cursor+allow-vo-dragging"}));
        for (;;) {
            const auto *event = mpv_wait_event(handle, 0.1);
            if (event->event_id == MPV_EVENT_SHUTDOWN)
                break;
            try {
                switch (event->event_id) {
                case MPV_EVENT_HOOK: {
                    const auto &hook = *static_cast<const mpv_event_hook *>(event->data);
                    struct Continue {
                        mpv_handle *handle;
                        uint64_t id;
                        ~Continue() {
                            mpv_hook_continue(handle, id);
                        }
                    } guard{handle, hook.id};
                    player.hook(event->reply_userdata == 24);
                    break;
                }
                case MPV_EVENT_START_FILE:
                    if (!player.loading() || player.mpv.text("path") != player.original.path) {
                        player.clear(false);
                        player.original = {};
                    }
                    break;
                case MPV_EVENT_END_FILE: {
                    const auto &end = *static_cast<const mpv_event_end_file *>(event->data);
                    if (end.error < 0 && player.phase == Phase::Restoring)
                        player.load(Phase::Refreshing);
                    else if (end.error < 0 && (player.phase == Phase::Processed ||
                                               player.phase == Phase::LoadingProcessed))
                        throw Error(503, mpv_error_string(end.error), "player_failed");
                    else if (!player.loading())
                        player.clear(false);
                    break;
                }
                case MPV_EVENT_FILE_LOADED:
                    player.loaded();
                    break;
                case MPV_EVENT_PROPERTY_CHANGE:
                    if (player.session)
                        player.session->paused(player.mpv.paused());
                    break;
                case MPV_EVENT_CLIENT_MESSAGE: {
                    const auto &message =
                        *static_cast<const mpv_event_client_message *>(event->data);
                    if (message.num_args > 0 && std::string(message.args[0]) == "toggle")
                        player.toggle();
                    break;
                }
                case MPV_EVENT_COMMAND_REPLY:
                    if (event->error < 0)
                        throw Error(503, mpv_error_string(event->error), "player_failed");
                    break;
                case MPV_EVENT_LOG_MESSAGE: {
                    const auto &message = *static_cast<const mpv_event_log_message *>(event->data);
                    log_event("mpv.warning", {{"component", message.prefix},
                                              {"level", message.level},
                                              {"message", message.text}});
                    break;
                }
                default:
                    break;
                }
                player.tick();
            } catch (const std::exception &error) {
                player.failed(error);
            }
        }
        log_event("plugin.stop");
    } catch (const std::exception &error) {
        log_event("plugin.failed", {{"error", error.what()}});
        Mpv(handle).osd("RIFE: " + std::string(error.what()), 10000);
    }
    return 0;
}
