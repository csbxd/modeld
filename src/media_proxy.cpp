#include "modeld/stream.hpp"
#include <algorithm>
#include <condition_variable>
#include <deque>
#include <future>
#include <map>
#include <mutex>
#include <thread>

namespace modeld {
struct MediaProxy::Cache {
    struct Saved {
        int64_t index;
        uint64_t used;
        std::shared_ptr<std::string> bytes;
    };
    std::string signature;
    std::vector<Saved> entries;
};
struct MediaProxy::Impl {
    using Bytes = std::shared_ptr<std::string>;
    struct Entry {
        std::promise<Bytes> promise;
        std::shared_future<Bytes> future = promise.get_future().share();
        bool completed = false, failed = false;
        uint64_t used = 0;
        size_t bytes = 0;
    };
    struct Task {
        int64_t index;
        uint64_t generation;
        std::shared_ptr<Entry> entry;
    };
    ApiClient api;
    Manifest manifest;
    httplib::Server server;
    std::thread listener;
    std::vector<std::thread> workers;
    std::vector<std::unique_ptr<httplib::Client>> clients;
    mutable std::mutex mutex;
    std::mutex seek_mutex;
    std::condition_variable condition;
    std::map<int64_t, std::shared_ptr<Entry>> entries;
    std::deque<Task> tasks;
    std::atomic<bool> stopping{false};
    std::atomic<uint64_t> generation{0};
    bool resetting = false;
    int64_t demand = -1, frontier = -1;
    uint64_t ticks = 0, cached_bytes = 0;
    int port = 0;
    std::string token = random_id(), init, signature;
    static constexpr int window = 6;
    explicit Impl(ApiClient a, Manifest m) : api(std::move(a)), manifest(std::move(m)) {}
    void fail(const std::shared_ptr<Entry> &entry, const Error &error) {
        if (!entry->completed) {
            entry->failed = entry->completed = true;
            entry->promise.set_exception(std::make_exception_ptr(error));
        }
    }
    std::shared_ptr<Entry> enqueue(int64_t index) {
        if (index < 0 || index >= manifest.segment_count)
            throw Error(404, "Segment does not exist");
        auto existing = entries.find(index);
        if (existing != entries.end() && !existing->second->failed) {
            existing->second->used = ++ticks;
            return existing->second;
        }
        auto entry = std::make_shared<Entry>();
        entry->used = ++ticks;
        entries[index] = entry;
        tasks.push_back({index, generation.load(), entry});
        condition.notify_all();
        return entry;
    }
    void prefetch() {
        for (auto next = frontier + 1;
             demand >= 0 && next < manifest.segment_count && next <= demand + window; ++next) {
            const auto cached = entries.find(next);
            if (cached != entries.end() && cached->second->completed && !cached->second->failed) {
                frontier = next;
                continue;
            }
            enqueue(next);
            break;
        }
    }
    void trim() {
        while (cached_bytes > api.config().cache_bytes) {
            auto oldest = entries.end();
            for (auto it = entries.begin(); it != entries.end(); ++it)
                if (it->second->completed && !it->second->failed &&
                    (oldest == entries.end() || it->second->used < oldest->second->used))
                    oldest = it;
            if (oldest == entries.end())
                break;
            cached_bytes -= oldest->second->bytes;
            entries.erase(oldest);
        }
    }
    void run(httplib::Client &client) {
        for (;;) {
            Task task;
            {
                std::unique_lock lock(mutex);
                condition.wait(lock, [&] { return stopping || (!resetting && !tasks.empty()); });
                if (stopping)
                    return;
                task = tasks.front();
                tasks.pop_front();
                condition.wait(lock, [&] {
                    return stopping || task.generation != generation || task.index <= frontier + 1;
                });
                if (stopping)
                    return;
                if (task.generation != generation)
                    continue;
            }
            auto bytes = std::make_shared<std::string>();
            Error error(503, "Media transfer failed", "transfer_failed");
            bool success = false;
            const auto started = Clock::now();
            for (int attempt = 0; attempt < 3; ++attempt) {
                bytes->clear();
                int status = 0;
                auto response = client.Get(
                    manifest.segment(task.index),
                    [&](const httplib::Response &header) {
                        status = header.status;
                        if (status != 200)
                            return true;
                        std::lock_guard lock(mutex);
                        if (stopping || task.generation != generation)
                            return false;
                        frontier = std::max(frontier, task.index);
                        prefetch();
                        condition.notify_all();
                        return true;
                    },
                    [&](const char *data, size_t length) {
                        if (stopping || task.generation != generation ||
                            bytes->size() + length > (64ULL << 20))
                            return false;
                        bytes->append(data, length);
                        return true;
                    });
                if (response)
                    status = response->status;
                success = response && status == 200 && !bytes->empty();
                if (success || stopping || task.generation != generation)
                    break;
                error = Error(status ? status : 503,
                              "Media transfer failed: " + httplib::to_string(response.error()),
                              "transfer_failed");
                auto payload = Json::parse(*bytes, nullptr, false);
                if (payload.is_object() && payload.contains("error")) {
                    const auto &body = payload["error"];
                    if (body.contains("message") && body["message"].is_string())
                        error =
                            Error(status, body["message"], body.value("code", "transfer_failed"));
                    error.retryable = body.value("retryable", error.retryable);
                }
                if (!error.retryable || attempt == 2)
                    break;
                log_event("media.retry", {{"index", task.index},
                                          {"attempt", attempt + 1},
                                          {"status", error.status},
                                          {"code", error.code}});
                std::unique_lock lock(mutex);
                condition.wait_for(lock, std::chrono::milliseconds(400 * (attempt + 1)),
                                   [&] { return stopping || task.generation != generation; });
            }
            std::lock_guard lock(mutex);
            if (task.entry->completed)
                continue;
            if (stopping || task.generation != generation)
                fail(task.entry, Error(499, "Transfer canceled", "canceled"));
            else if (!success) {
                log_event("media.failed", {{"index", task.index},
                                           {"status", error.status},
                                           {"code", error.code},
                                           {"message", error.what()}});
                fail(task.entry, error);
            } else {
                task.entry->completed = true;
                task.entry->bytes = bytes->size();
                task.entry->used = ++ticks;
                cached_bytes += bytes->size();
                task.entry->promise.set_value(bytes);
                trim();
                log_event("media.complete",
                          {{"index", task.index},
                           {"bytes", bytes->size()},
                           {"elapsed_ms", std::chrono::duration_cast<std::chrono::milliseconds>(
                                              Clock::now() - started)
                                              .count()}});
            }
        }
    }
    Bytes fetch(int64_t index) {
        std::unique_lock seek_lock(seek_mutex);
        std::shared_ptr<Entry> entry;
        bool reset = false;
        {
            std::lock_guard lock(mutex);
            if (stopping)
                throw Error(410, "Stream closed", "session_closed");
            if (index < 0 || index >= manifest.segment_count)
                throw Error(404, "Segment does not exist");
            if (demand >= 0 && (index < demand - window || index > demand + window)) {
                ++generation;
                resetting = true;
                reset = true;
                frontier = index - 1;
                for (auto it = entries.begin(); it != entries.end();) {
                    if (!it->second->completed || it->second->failed) {
                        fail(it->second, Error(499, "Seek replaced this transfer", "canceled"));
                        it = entries.erase(it);
                    } else
                        ++it;
                }
                tasks.clear();
                log_event("media.seek", {{"from", demand}, {"to", index}});
            }
            demand = index;
            entry = enqueue(index);
            if (entry->completed && !entry->failed) {
                frontier = std::max(frontier, index);
                log_event("media.cache.hit", {{"index", index}, {"bytes", cached_bytes}});
            }
            prefetch();
        }
        if (reset) {
            for (auto &client : clients)
                client->stop();
            std::lock_guard lock(mutex);
            resetting = false;
        }
        condition.notify_all();
        seek_lock.unlock();
        return entry->future.get();
    }
    ~Impl() {
        stopping = true;
        {
            std::lock_guard lock(mutex);
            for (auto &[index, entry] : entries)
                fail(entry, Error(410, "Stream closed", "session_closed"));
        }
        condition.notify_all();
        for (auto &client : clients)
            client->stop();
        server.stop();
        for (auto &worker : workers)
            if (worker.joinable())
                worker.join();
        if (listener.joinable())
            listener.join();
    }
};
MediaProxy::MediaProxy(const ApiClient &api, Manifest manifest, std::stop_token stop,
                       std::shared_ptr<Cache> cache)
    : impl(std::make_unique<Impl>(api, std::move(manifest))) {
    auto probe = api.connection();
    probe->set_read_timeout(api.config().request_timeout_seconds);
    probe->set_payload_max_length(64ULL << 20);
    std::stop_callback cancel(stop, [&] { probe->stop(); });
    if (stop.stop_requested())
        throw Error(499, "Stream initialization canceled", "canceled");
    auto header = probe->Get(impl->manifest.init);
    if (!header || header->status != 200 || header->body.empty())
        throw Error(503, "Cannot read Matroska initialization", "initialization_failed");
    impl->init = std::move(header->body);
    impl->signature = impl->manifest.media.dump() + impl->init;
    if (cache && cache->signature == impl->signature) {
        std::sort(cache->entries.begin(), cache->entries.end(),
                  [](const auto &a, const auto &b) { return a.used < b.used; });
        for (const auto &saved : cache->entries) {
            if (saved.index >= impl->manifest.segment_count)
                continue;
            auto entry = std::make_shared<Impl::Entry>();
            entry->completed = true;
            entry->used = ++impl->ticks;
            entry->bytes = saved.bytes->size();
            entry->promise.set_value(saved.bytes);
            impl->entries[saved.index] = entry;
            impl->cached_bytes += entry->bytes;
        }
        impl->trim();
        log_event("media.cache.reused",
                  {{"segments", impl->entries.size()}, {"bytes", impl->cached_bytes}});
    }
    const auto path = "/stream/" + impl->token;
    impl->server.Get(path + "/timeline.edl", [this, path](const auto &, auto &response) {
        response.set_content(
            impl->manifest.edl("http://127.0.0.1:" + std::to_string(impl->port) + path),
            "text/plain");
    });
    impl->server.Get(path + "/init", [this](const auto &, auto &response) {
        response.set_content(impl->init, "video/x-matroska");
    });
    impl->server.Get(path + R"(/segments/([0-9]{1,10}))",
                     [this](const auto &request, auto &response) {
                         try {
                             auto bytes = impl->fetch(std::stoll(request.matches[1]));
                             response.set_content_provider(
                                 bytes->size(), "video/x-matroska",
                                 [bytes](size_t offset, size_t length, httplib::DataSink &sink) {
                                     return sink.write(bytes->data() + offset, length);
                                 });
                         } catch (const Error &e) {
                             response.status = e.status;
                             response.set_content(e.what(), "text/plain");
                         } catch (const std::exception &e) {
                             response.status = 503;
                             response.set_content(e.what(), "text/plain");
                         }
                     });
    impl->server.new_task_queue = [] { return new httplib::ThreadPool(8); };
    impl->port = impl->server.bind_to_any_port("127.0.0.1");
    if (impl->port < 0)
        throw Error(503, "Cannot bind local stream proxy");
    for (int i = 0; i < 4; ++i) {
        auto client = api.connection();
        client->set_keep_alive(true);
        client->set_read_timeout(api.config().request_timeout_seconds);
        auto *pointer = client.get();
        impl->clients.push_back(std::move(client));
        impl->workers.emplace_back([this, pointer] { impl->run(*pointer); });
    }
    impl->listener = std::thread([this] { impl->server.listen_after_bind(); });
}
MediaProxy::~MediaProxy() = default;
std::string MediaProxy::url() const {
    return "http://127.0.0.1:" + std::to_string(impl->port) + "/stream/" + impl->token +
           "/timeline.edl";
}
std::shared_ptr<MediaProxy::Cache> MediaProxy::saved_cache() const {
    auto cache = std::make_shared<Cache>();
    std::lock_guard lock(impl->mutex);
    cache->signature = impl->signature;
    for (const auto &[index, entry] : impl->entries)
        if (entry->completed && !entry->failed)
            cache->entries.push_back({index, entry->used, entry->future.get()});
    return cache;
}
} // namespace modeld
