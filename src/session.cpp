#include "modeld/session.hpp"
#include <condition_variable>
#include <mutex>
#include <thread>
namespace modeld {
struct RemoteSession::Impl {
    ApiClient api;
    std::string id;
    int lease_seconds;
    std::mutex mutex;
    std::condition_variable_any condition;
    bool paused = false, changed = true;
    std::jthread worker;
    Impl(ApiClient client, std::string identifier, int lease)
        : api(std::move(client)), id(std::move(identifier)), lease_seconds(lease) {
        worker = std::jthread([this](std::stop_token stop) {
            while (!stop.stop_requested()) {
                bool value;
                {
                    std::unique_lock lock(mutex);
                    condition.wait_for(lock, stop,
                                       std::chrono::seconds(std::max(1, lease_seconds / 3)),
                                       [&] { return changed; });
                    if (stop.stop_requested())
                        return;
                    value = paused;
                    changed = false;
                }
                try {
                    api.request("PUT", std::string(api_prefix) + "/sessions/" + id + "/lease",
                                {{"paused", value}}, stop);
                } catch (const std::exception &e) {
                    if (!stop.stop_requested())
                        log_event("lease.failed", {{"error", e.what()}});
                }
            }
        });
    }
    ~Impl() {
        worker.request_stop();
        condition.notify_all();
        if (worker.joinable())
            worker.join();
        try {
            api.request("DELETE", std::string(api_prefix) + "/sessions/" + id);
        } catch (const std::exception &e) {
            log_event("session.close.failed", {{"error", e.what()}});
        }
    }
};
RemoteSession::RemoteSession(ApiClient api, const Json &created)
    : info(SessionInfo::parse(created)) {
    impl = std::make_unique<Impl>(std::move(api), info.id, info.lease_seconds);
}
RemoteSession::~RemoteSession() {
    impl.reset();
    stream.reset();
}
void RemoteSession::paused(bool value) {
    {
        std::lock_guard lock(impl->mutex);
        if (impl->paused == value)
            return;
        impl->paused = value;
        impl->changed = true;
    }
    impl->condition.notify_all();
}
struct OpenSession::Impl {
    mutable std::mutex mutex;
    std::string stage = "input.resolve";
    std::unique_ptr<RemoteSession> result;
    std::exception_ptr error;
    bool done = false;
    std::jthread worker;
    ~Impl() {
        worker.request_stop();
        if (worker.joinable())
            worker.join();
    }
    void phase(std::string value) {
        std::lock_guard lock(mutex);
        stage = std::move(value);
    }
};
OpenSession::OpenSession(ClientConfig config, std::string webpage, OpenRequest request,
                         std::shared_ptr<MediaProxy::Cache> cache)
    : impl(std::make_unique<Impl>()) {
    auto *state = impl.get();
    state->worker = std::jthread([state, config = std::move(config), webpage = std::move(webpage),
                                  request = std::move(request),
                                  cache = std::move(cache)](std::stop_token stop) mutable {
        ApiClient api(config);
        Json created;
        std::unique_ptr<RemoteSession> session;
        try {
            request.video.urls = resolve_sources(webpage, request.video.urls.at(0), stop);
            if (request.audio)
                request.audio->urls = resolve_sources(webpage, request.audio->urls.at(0), stop);
            if (stop.stop_requested())
                throw Error(499, "Session opening canceled", "canceled");
            state->phase("session.open");
            const auto capacity_deadline =
                Clock::now() + std::chrono::seconds(config.request_timeout_seconds);
            for (;;) {
                try {
                    created = api.request("POST", std::string(api_prefix) + "/sessions",
                                          request.json(), stop);
                    break;
                } catch (const Error &error) {
                    if (error.code != "session_limit" || !error.retryable ||
                        Clock::now() >= capacity_deadline)
                        throw;
                    state->phase("session.wait_capacity");
                    std::mutex mutex;
                    std::condition_variable_any condition;
                    std::unique_lock lock(mutex);
                    condition.wait_for(lock, stop, std::chrono::milliseconds(250),
                                       [] { return false; });
                    if (stop.stop_requested())
                        throw Error(499, "Session opening canceled", "canceled");
                }
            }
            session = std::make_unique<RemoteSession>(api, created);
            if (stop.stop_requested())
                throw Error(499, "Session opening canceled", "canceled");
            state->phase("stream.initialize");
            session->stream =
                std::make_unique<MediaProxy>(api, session->info.manifest, stop, cache);
            {
                std::lock_guard lock(state->mutex);
                state->result = std::move(session);
            }
        } catch (...) {
            state->error = std::current_exception();
            if (!session && created.is_object() && created.contains("id") &&
                created["id"].is_string()) {
                const auto id = created["id"].get<std::string>();
                if (id.size() == 32 &&
                    id.find_first_not_of("0123456789abcdef") == std::string::npos)
                    try {
                        api.request("DELETE", std::string(api_prefix) + "/sessions/" + id);
                    } catch (...) {
                    }
            }
        }
        std::lock_guard lock(state->mutex);
        state->done = true;
    });
}
OpenSession::~OpenSession() = default;
bool OpenSession::ready() const {
    std::lock_guard lock(impl->mutex);
    return impl->done;
}
std::string OpenSession::stage() const {
    std::lock_guard lock(impl->mutex);
    return impl->stage;
}
std::unique_ptr<RemoteSession> OpenSession::take() {
    std::lock_guard lock(impl->mutex);
    if (!impl->done)
        throw Error(409, "Session is not ready");
    if (impl->error)
        std::rethrow_exception(impl->error);
    if (!impl->result)
        throw Error(409, "Session was already taken");
    return std::move(impl->result);
}
} // namespace modeld
