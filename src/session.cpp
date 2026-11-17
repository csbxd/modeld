#include "modeld/common.hpp"
#include <mutex>
#include <thread>

namespace modeld {
struct SessionRequest::Impl {
    Json config;
    mutable std::mutex mutex;
    std::string current_stage = "source.alternatives";
    SessionResult result;
    std::exception_ptr error;
    bool done = false, taken = false;
    std::jthread worker;

    void stage(const char *value) {
        std::lock_guard lock(mutex);
        current_stage = value;
    }
    ~Impl() {
        worker.request_stop();
        if (worker.joinable())
            worker.join();
        // Cancellation may race a successful POST or playlist download. Close
        // any session that was created but never transferred to the player.
        if (!taken && result.info.is_object() && result.info.contains("id")) {
            try {
                log_event("session.abandon");
                request_json(config, "DELETE",
                             "/v1/sessions/" + result.info.at("id").get<std::string>());
            } catch (const std::exception &e) {
                log_event("session.abandon.failed", {{"error", e.what()}});
            }
        }
    }
};

SessionRequest::SessionRequest(const Json &config, const std::string &webpage,
                               const std::string &source, const Json &headers)
    : impl(std::make_unique<Impl>()) {
    impl->config = config;
    auto *state = impl.get();
    state->worker = std::jthread([state, webpage, source, headers](std::stop_token stop) {
        try {
            auto urls = resolve_video_sources(webpage, source, stop);
            if (stop.stop_requested())
                throw Error(499, "Session creation canceled");
            state->stage("session.create");
            Json backups = Json::array(), names = Json::array();
            for (size_t n = 1; n < urls.size(); ++n)
                backups.push_back(urls[n]);
            for (const auto &[name, value] : headers.items())
                names.push_back(name);
            log_event("session.create", {{"source", log_url(urls.front())},
                                         {"backup_count", backups.size()},
                                         {"header_names", names}});
            state->result.info = request_json(state->config, "POST", "/v1/sessions",
                                              {{"model", model_id},
                                               {"task", "frame_interpolation"},
                                               {"source_url", urls.front()},
                                               {"source_backups", backups},
                                               {"source_headers", headers},
                                               {"target_fps", 60}},
                                              stop);
            if (stop.stop_requested())
                throw Error(499, "Session creation canceled");
            const auto &info = state->result.info;
            log_event("session.created", {{"width", info.at("width")},
                                          {"height", info.at("height")},
                                          {"source_fps", info.at("source_fps")},
                                          {"duration", info.at("duration")}});
            state->stage("media.timeline");
            state->result.media = std::make_unique<MediaProxy>(
                info.at("playlist_url").get<std::string>(), info.at("duration").get<double>(),
                state->config, stop);
        } catch (...) {
            state->error = std::current_exception();
        }
        std::lock_guard lock(state->mutex);
        state->done = true;
    });
}
SessionRequest::~SessionRequest() = default;
bool SessionRequest::ready() const {
    std::lock_guard lock(impl->mutex);
    return impl->done;
}
std::string SessionRequest::stage() const {
    std::lock_guard lock(impl->mutex);
    return impl->current_stage;
}
SessionResult SessionRequest::take() {
    std::lock_guard lock(impl->mutex);
    if (!impl->done || impl->taken)
        throw Error(409, "Session creation is not ready");
    if (impl->error)
        std::rethrow_exception(impl->error);
    impl->taken = true;
    return std::move(impl->result);
}
} // namespace modeld
