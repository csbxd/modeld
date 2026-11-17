#include "modeld/common.hpp"
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <future>
#include <iomanip>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>

namespace modeld {
struct MediaProxy::Cache {
    struct Saved {
        int64_t index;
        uint64_t used;
        std::shared_ptr<std::string> bytes;
    };
    std::string init;
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
    httplib::Server server;
    std::thread listener;
    std::vector<std::thread> workers;
    std::vector<std::shared_ptr<httplib::Client>> clients;
    mutable std::mutex mutex;
    std::mutex demand_mutex;
    std::condition_variable cv;
    std::deque<Task> tasks;
    std::map<int64_t, std::shared_ptr<Entry>> entries;
    std::set<int64_t> completed;
    std::atomic<bool> stopping{false};
    std::atomic<uint64_t> generation{0};
    bool resetting = false;
    int64_t demand = -1, frontier = -1, count = 0;
    int seconds = 2, port = 0;
    double duration = 0;
    std::string origin, prefix, token, playlist;
    std::string extension = ".ts", init;
    uint64_t ticks = 0, cached_bytes = 0, cache_limit = 256ULL << 20;
    std::string failure;
    static constexpr int window = 6;

    void fail_locked(const std::shared_ptr<Entry> &entry, const char *message) {
        if (!entry->completed) {
            entry->failed = entry->completed = true;
            entry->promise.set_exception(std::make_exception_ptr(Error(503, message)));
        }
    }
    std::shared_ptr<Entry> enqueue_locked(int64_t index) {
        if (index < 0 || index >= count)
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
        cv.notify_all();
        return entry;
    }
    void prefetch_locked() {
        for (auto next = frontier + 1; demand >= 0 && next < count && next <= demand + window;
             ++next) {
            auto cached = entries.find(next);
            if (cached != entries.end() && cached->second->completed && !cached->second->failed) {
                frontier = next;
                continue;
            }
            enqueue_locked(next);
            break;
        }
    }
    void trim_locked() {
        while (cached_bytes > cache_limit) {
            auto oldest = entries.end();
            for (auto it = entries.begin(); it != entries.end(); ++it)
                if (it->second->completed && !it->second->failed && it->first != demand &&
                    (oldest == entries.end() || it->second->used < oldest->second->used))
                    oldest = it;
            if (oldest == entries.end())
                break;
            cached_bytes -= oldest->second->bytes;
            completed.erase(oldest->first);
            entries.erase(oldest);
        }
    }
    void run(const std::shared_ptr<httplib::Client> &client) {
        for (;;) {
            Task task;
            {
                std::unique_lock lock(mutex);
                cv.wait(lock, [&] { return stopping || (!resetting && !tasks.empty()); });
                if (stopping)
                    return;
                task = tasks.front();
                tasks.pop_front();
                // A later segment starts only after the previous response headers.
                // The server has already queued that contiguous GPU job at this point.
                cv.wait(lock, [&] {
                    return stopping || task.generation != generation || task.index <= frontier + 1;
                });
                if (stopping)
                    return;
                if (task.generation != generation)
                    continue;
            }
            auto bytes = std::make_shared<std::string>();
            const auto started = Clock::now();
            int status = 0;
            bool transferred = false;
            std::string error;
            for (int attempt = 0; attempt < 3; ++attempt) {
                bytes->clear();
                status = 0;
                auto response = client->Get(
                    prefix + "segments/" + std::to_string(task.index) + extension,
                    [&](const httplib::Response &header) {
                        status = header.status;
                        if (header.status != 200)
                            return true;
                        std::lock_guard lock(mutex);
                        if (stopping || task.generation != generation)
                            return false;
                        frontier = std::max(frontier, task.index);
                        prefetch_locked();
                        cv.notify_all();
                        return true;
                    },
                    [&](const char *data, size_t size) {
                        if (stopping || task.generation != generation ||
                            bytes->size() + size > 64 * 1024 * 1024)
                            return false;
                        if (status == 200)
                            bytes->append(data, size);
                        return true;
                    });
                if (response)
                    status = response->status;
                error = httplib::to_string(response.error());
                transferred = response && status == 200;
                if (transferred || stopping || task.generation != generation)
                    break;
                const bool transient = status == 0 || status == 408 || status == 429 ||
                                       status == 500 || status == 502 || status == 503 ||
                                       status == 504;
                if (!transient || attempt == 2)
                    break;
                log_event("media.segment.retry", {{"index", task.index},
                                                  {"status", status},
                                                  {"attempt", attempt + 1},
                                                  {"error", error}});
                std::unique_lock lock(mutex);
                cv.wait_for(lock, std::chrono::milliseconds(400 * (attempt + 1)),
                            [&] { return stopping || task.generation != generation; });
            }
            {
                std::lock_guard lock(mutex);
                if (task.entry->completed)
                    continue;
                if (stopping || task.generation != generation || !transferred) {
                    if (!stopping && task.generation == generation) {
                        log_event("media.segment.failed",
                                  {{"index", task.index}, {"status", status}, {"error", error}});
                        failure = "远端视频分段读取失败（HTTP " + std::to_string(status) + "）";
                    }
                    fail_locked(task.entry, "Remote video transfer failed");
                } else {
                    log_event("media.segment.complete",
                              {{"index", task.index},
                               {"bytes", bytes->size()},
                               {"elapsed_ms", std::chrono::duration_cast<std::chrono::milliseconds>(
                                                  Clock::now() - started)
                                                  .count()}});
                    task.entry->completed = true;
                    task.entry->bytes = bytes->size();
                    task.entry->used = ++ticks;
                    cached_bytes += bytes->size();
                    completed.insert(task.index);
                    task.entry->promise.set_value(bytes);
                    trim_locked();
                }
            }
        }
    }
    Bytes fetch(int64_t index) {
        std::unique_lock demand_lock(demand_mutex);
        std::shared_ptr<Entry> entry;
        bool reset = false;
        {
            std::lock_guard lock(mutex);
            if (stopping)
                throw Error(410, "Video proxy was closed");
            if (index < 0 || index >= count)
                throw Error(404, "Segment does not exist");
            auto cached = entries.find(index);
            bool ready =
                cached != entries.end() && cached->second->completed && !cached->second->failed;
            if (demand >= 0 && (index < demand - window || index > demand + window)) {
                log_event("media.seek", {{"from_segment", demand}, {"to_segment", index}});
                generation++;
                resetting = true;
                std::erase_if(entries, [&](const auto &pair) {
                    const auto &e = pair.second;
                    if (!e->completed || e->failed) {
                        fail_locked(e, "Video seek superseded this transfer");
                        return true;
                    }
                    return false;
                });
                tasks.clear();
                failure.clear();
                frontier = index - 1;
                reset = true;
            }
            demand = index;
            entry = enqueue_locked(index);
            if (ready) {
                frontier = std::max(frontier, index);
                log_event("media.cache.hit", {{"index", index}, {"cached_bytes", cached_bytes}});
            }
            prefetch_locked();
        }
        if (reset) {
            for (auto &client : clients)
                client->stop();
            std::lock_guard lock(mutex);
            resetting = false;
        }
        cv.notify_all();
        demand_lock.unlock();
        return entry->future.get();
    }
    ~Impl() {
        stopping = true;
        {
            std::lock_guard lock(mutex);
            for (auto &[n, e] : entries)
                fail_locked(e, "Video transfer was closed");
        }
        cv.notify_all();
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
MediaProxy::MediaProxy(const std::string &remote, double duration, const Json &config,
                       std::stop_token stop, std::shared_ptr<Cache> cache)
    : impl(std::make_unique<Impl>()) {
    log_event("media.timeline.request", {{"origin", log_url(remote)}, {"duration", duration}});
    auto resolved = media_url(remote, config);
    auto slash = resolved.find('/', 7);
    if (!resolved.starts_with("http://") || slash == std::string::npos)
        throw Error(422, "An HTTP modeld media URL is required");
    impl->origin = resolved.substr(0, slash);
    impl->prefix = resolved.substr(slash, resolved.rfind('/') - slash + 1);
    impl->duration = duration;
    impl->token = random_id();
    httplib::Client probe(impl->origin);
    configure_api_client(probe, config);
    probe.set_connection_timeout(5);
    probe.set_read_timeout(5);
    probe.set_max_timeout(10000);
    probe.set_payload_max_length(2 * 1024 * 1024);
    std::stop_callback cancel_probe(stop, [&] { probe.stop(); });
    if (stop.stop_requested())
        throw Error(499, "Session creation canceled");
    auto response = probe.Get(impl->prefix + "index.m3u8");
    log_event("media.timeline.response",
              {{"status", response ? response->status : 0},
               {"bytes", response ? response->body.size() : 0},
               {"m3u8", response && response->body.starts_with("#EXTM3U")},
               {"error", httplib::to_string(response.error())}});
    if (!response || response->status != 200 || !response->body.starts_with("#EXTM3U"))
        throw Error(503, "Cannot read remote video timeline");
    impl->playlist = response->body;
    impl->cache_limit = config.value("media_cache_bytes", uint64_t{256ULL << 20});
    if (impl->cache_limit < 1 || impl->cache_limit > (2ULL << 30))
        throw Error(422, "media_cache_bytes must be between 1 and 2147483648");
    if (impl->playlist.find("#EXT-X-MAP:URI=\"init.mkv\"") != std::string::npos) {
        impl->extension = ".mkv";
        probe.set_payload_max_length(64 * 1024 * 1024);
        auto header = probe.Get(impl->prefix + "init.mkv");
        if (!header || header->status != 200)
            throw Error(503, "Cannot read media initialization header");
        impl->init = std::move(header->body);
    }
    if (cache && cache->init == impl->init && !impl->init.empty()) {
        std::sort(cache->entries.begin(), cache->entries.end(),
                  [](const auto &a, const auto &b) { return a.used < b.used; });
        for (const auto &saved : cache->entries) {
            auto entry = std::make_shared<Impl::Entry>();
            entry->completed = true;
            entry->bytes = saved.bytes->size();
            entry->used = ++impl->ticks;
            entry->promise.set_value(saved.bytes);
            impl->entries[saved.index] = entry;
            impl->completed.insert(saved.index);
            impl->cached_bytes += entry->bytes;
        }
        impl->trim_locked();
        log_event("media.cache.reused",
                  {{"segments", impl->entries.size()}, {"bytes", impl->cached_bytes}});
    }
    std::istringstream lines(impl->playlist);
    std::string line;
    while (std::getline(lines, line))
        if (line.starts_with("#EXT-X-TARGETDURATION:"))
            impl->seconds = std::stoi(line.substr(22));
    if (impl->seconds < 1 || impl->seconds > 10 || !std::isfinite(duration) || duration <= 0)
        throw Error(502, "Invalid remote timeline");
    impl->count = static_cast<int64_t>(std::ceil(duration / impl->seconds));
    log_event("media.timeline.ready",
              {{"segment_seconds", impl->seconds}, {"segments", impl->count}});
    const auto path = "/play/" + impl->token;
    impl->server.Get(path + "/index.m3u8", [this](const auto &, auto &res) {
        res.set_content(impl->playlist, "application/vnd.apple.mpegurl");
    });
    impl->server.Get(path + "/index.edl", [this, path](const auto &, auto &res) {
        if (impl->init.empty()) {
            res.status = 404;
            return;
        }
        const auto base = "http://127.0.0.1:" + std::to_string(impl->port) + path;
        auto quoted = [](const std::string &value) {
            return "%" + std::to_string(value.size()) + "%" + value;
        };
        std::ostringstream edl;
        // mpv's DASH timeline provides a single demuxer/cache for all tracks.
        // The init prefix mechanism also works for Matroska clusters, preserving
        // audio/subtitle codecs that fragmented MP4 cannot carry.
        edl << "# mpv EDL v0\n!no_chapters\n!mp4_dash,init=" << quoted(base + "/init.mkv") << '\n'
            << std::fixed << std::setprecision(9);
        for (int64_t index = 0; index < impl->count; ++index) {
            const auto begin = index * static_cast<double>(impl->seconds);
            const auto end = std::min(std::ceil(impl->duration * 60) / 60, begin + impl->seconds);
            edl << quoted(base + "/segments/" + std::to_string(index) + impl->extension)
                << ",length=" << end - begin << '\n';
        }
        res.set_content(edl.str(), "text/plain");
    });
    impl->server.Get(path + "/init.mkv", [this](const auto &, auto &res) {
        if (impl->init.empty()) {
            res.status = 404;
            return;
        }
        res.set_content(impl->init, "video/x-matroska");
    });
    impl->server.Get(path + R"(/segments/([0-9]{1,9})\.)" + impl->extension.substr(1),
                     [this](const auto &req, auto &res) {
                         try {
                             auto bytes = impl->fetch(std::stoll(req.matches[1]));
                             res.set_content(*bytes, impl->extension == ".mkv" ? "video/x-matroska"
                                                                               : "video/MP2T");
                         } catch (const Error &e) {
                             res.status = e.status;
                             res.set_content(e.what(), "text/plain");
                         } catch (const std::exception &e) {
                             res.status = 503;
                             res.set_content(e.what(), "text/plain");
                         }
                     });
    impl->server.new_task_queue = [] { return new httplib::ThreadPool(8); };
    impl->port = impl->server.bind_to_any_port("127.0.0.1");
    if (impl->port < 0)
        throw Error(503, "Cannot bind local video proxy");
    for (int i = 0; i < 4; i++) {
        auto client = std::make_shared<httplib::Client>(impl->origin);
        configure_api_client(*client, config);
        client->set_keep_alive(true);
        client->set_connection_timeout(10);
        client->set_read_timeout(3600);
        impl->clients.push_back(client);
        impl->workers.emplace_back([this, client] { impl->run(client); });
    }
    impl->listener = std::thread([this] { impl->server.listen_after_bind(); });
}
MediaProxy::~MediaProxy() = default;
std::string MediaProxy::url() const {
    return "http://127.0.0.1:" + std::to_string(impl->port) + "/play/" + impl->token +
           (impl->init.empty() ? "/index.m3u8" : "/index.edl");
}
double MediaProxy::buffered_ahead(double position) const {
    if (!std::isfinite(position) || position < 0)
        return 0;
    std::lock_guard lock(impl->mutex);
    auto index = static_cast<int64_t>(position / impl->seconds);
    auto last = index;
    while (impl->completed.contains(last))
        last++;
    return std::max(0.0,
                    std::min(impl->duration, last * static_cast<double>(impl->seconds)) - position);
}
std::string MediaProxy::failure() const {
    std::lock_guard lock(impl->mutex);
    return impl->failure;
}
std::shared_ptr<MediaProxy::Cache> MediaProxy::saved_cache() const {
    auto saved = std::make_shared<Cache>();
    std::lock_guard lock(impl->mutex);
    saved->init = impl->init;
    for (const auto &[index, entry] : impl->entries)
        if (entry->completed && !entry->failed)
            saved->entries.push_back({index, entry->used, entry->future.get()});
    return saved;
}
} // namespace modeld
