#include "modeld/session.hpp"
#include <atomic>
#include <cmath>
#include <fstream>
#include <future>
#include <iostream>
#include <map>
#include <mutex>
#include <sys/stat.h>
#include <thread>
namespace {
using namespace modeld;
void check(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
struct TempFile {
    std::filesystem::path path =
        std::filesystem::temp_directory_path() / ("modeld-test-" + random_id());
    explicit TempFile(const std::string &text) {
        std::ofstream f(path);
        f << text;
        ::chmod(path.c_str(), 0600);
    }
    ~TempFile() {
        std::filesystem::remove(path);
    }
};
struct Server {
    httplib::Server http;
    int port = http.bind_to_any_port("127.0.0.1");
    std::thread listener;
    void start() {
        listener = std::thread([this] { http.listen_after_bind(); });
    }
    ~Server() {
        http.stop();
        if (listener.joinable())
            listener.join();
    }
};
const std::string id(32, 'a');
Json manifest(double duration = 32) {
    return {{"schema", 2},
            {"media",
             {{"width", 320},
              {"height", 180},
              {"duration_seconds", duration},
              {"source_fps", 30},
              {"output_fps", 60},
              {"segment_seconds", 2},
              {"segment_count", static_cast<int>(std::ceil(std::ceil(duration * 60) / 120))},
              {"tracks", Json::array()}}},
            {"init", "/api/v2/sessions/" + id + "/media/init"},
            {"segment_template", "/api/v2/sessions/" + id + "/media/segments/{index}"}};
}
ClientConfig config(int port, const std::filesystem::path &token) {
    return ClientConfig::parse({{"endpoint", "http://unresolvable.invalid:" + std::to_string(port)},
                                {"connect_address", "127.0.0.1"},
                                {"token_file", token.string()},
                                {"source_bind", "127.0.0.1"}});
}
void protocol() {
    check(random_id().size() == 32 && random_id() != random_id(),
          "capability IDs must be unpredictable");
    check(supported_dimensions(1920, 1080) && supported_dimensions(1080, 1920) &&
              !supported_dimensions(1920, 1920),
          "one portrait/landscape pixel budget");
    auto m = Manifest::parse(manifest(4.01));
    const auto edl = m.edl("http://127.0.0.1/local");
    check(edl.find("!mp4_dash,init=") != std::string::npos &&
              edl.find("length=0.016666667") != std::string::npos,
          "fractional last frame is retained");
    for (auto bad : {Json{{"api", "http://old.example"}, {"token_file", "/test"}},
                     Json{{"endpoint", "http://valid.example"},
                          {"token_file", "/test"},
                          {"output_mode", "main"}}}) {
        bool rejected = false;
        try {
            ClientConfig::parse(bad);
        } catch (...) {
            rejected = true;
        }
        check(rejected, "configuration must reject unknown fields");
    }
    auto bad = manifest();
    bad["init"] = "https://foreign.example/private";
    bool rejected = false;
    try {
        Manifest::parse(bad);
    } catch (...) {
        rejected = true;
    }
    check(rejected, "media requests must stay on the configured API origin");
    const std::string video = "https://cdn.example/video.m4s?key=a,b;part=1";
    auto quote = [](const std::string &s) { return "%" + std::to_string(s.size()) + "%" + s; };
    const std::string dash =
        "edl://!new_stream;!no_clip;!delay_open,media_type=video;"
        "!track_meta,program_id=0;" +
        quote(video) +
        ",length=12;"
        "!new_stream;!no_clip;!delay_open,media_type=audio;https://cdn.example/a.m4s;"
        "!new_stream;!no_clip;!delay_open,media_type=audio;https://cdn.example/b.m4s";
    check(resolved_track_url(dash, {{"program-id", 0}}, "video") == video,
          "selected DASH video is exact");
    check(resolved_track_url(dash, {{"id", 2}}, "audio") == "https://cdn.example/b.m4s",
          "selected DASH audio is independent");
    const std::string original =
        "https://peer.mcdn.bilivideo.cn:8082/upgcxcode/a/42-1-30280.m4s?peer";
    const std::string backup =
        "https://upos-sz-mirrorhw.bilivideo.com/upgcxcode/a/42-1-30280.m4s?cdn";
    auto sources = bilibili_sources(
        original,
        {{"dash",
          {{"audio",
            Json::array(
                {{{"baseUrl", original},
                  {"backupUrl",
                   Json::array({backup, "https://foreign.example/upgcxcode/a/42-1-30280.m4s",
                                "https://upos-sz-mirrorhw.bilivideo.com/upgcxcode/a/"
                                "42-1-30216.m4s"})}}})}}}});
    check(sources == std::vector<std::string>({backup, original}),
          "audio fallback preserves the exact encoding");
}
void range() {
    TempFile file("0123456789");
    SourceServer source(file.path, "127.0.0.1");
    const auto url = source.url(), split = url.substr(0, url.find('/', 7));
    httplib::Client client(split);
    const auto r = client.Get(url.substr(split.size()), httplib::Headers{{"Range", "bytes=2-5"}});
    check(r && r->status == 206 && r->body == "2345", "source supports exact HTTP byte ranges");
    const auto denied = client.Get("/source/not-shared");
    check(denied && denied->status == 404, "source server exposes only its capability");
}
void cache() {
    TempFile token(random_id() + random_id());
    Server server;
    std::atomic<int> first{0};
    const auto base = "/api/v2/sessions/" + id + "/media";
    server.http.Get(base + "/init", [](const auto &, auto &r) {
        r.set_content("stable-tracks", "video/x-matroska");
    });
    server.http.Get(base + R"(/segments/([0-9]+))", [&](const auto &q, auto &r) {
        if (q.matches[1] == "0" && ++first <= 2) {
            r.status = 503;
            r.set_content(
                Json{{"error",
                      {{"code", "source_failed"}, {"message", "temporary"}, {"retryable", true}}}}
                    .dump(),
                "application/json");
            return;
        }
        r.set_content("cluster-" + q.matches[1].str(), "video/x-matroska");
    });
    server.start();
    const ApiClient api(config(server.port, token.path));
    std::shared_ptr<MediaProxy::Cache> saved;
    for (int pass = 0; pass < 2; ++pass) {
        MediaProxy proxy(api, Manifest::parse(manifest()), {}, saved);
        const auto url = proxy.url();
        const auto slash = url.find('/', 7);
        httplib::Client local(url.substr(0, slash));
        auto path = url.substr(slash);
        path.erase(path.rfind('/') + 1);
        const auto first_chunk = local.Get(path + "segments/0");
        check(first_chunk && first_chunk->body == "cluster-0" && first == 3,
              "transient errors retry; completed bytes are reused across toggles");
        const auto far = local.Get(path + "segments/12");
        check(far && far->body == "cluster-12", "far seek cancels pending old requests");
        const auto back = local.Get(path + "segments/0");
        check(back && back->body == "cluster-0" && first == 3,
              "completed cache survives distant seeks");
        const auto invalid = local.Get(path + "segments/99");
        check(invalid && invalid->status == 404, "out-of-range chunks are rejected");
        saved = proxy.saved_cache();
    }
}
void canceled_open() {
    TempFile token(random_id() + random_id());
    Server server;
    const auto base = "/api/v2/sessions/" + id;
    std::promise<void> reading;
    std::atomic<bool> release{false};
    std::atomic<int> closed{0};
    std::atomic<int> opening_attempts{0};
    server.http.Post("/api/v2/sessions", [&](const auto &q, auto &r) {
        if (++opening_attempts == 1) {
            r.status = 429;
            r.set_content(Json{{"error",
                                {{"code", "session_limit"},
                                 {"message", "Cancellation is finishing"},
                                 {"retryable", true}}}}
                              .dump(),
                          "application/json");
            return;
        }
        const auto body = Json::parse(q.body);
        check(body.at("video").contains("urls") && !body.contains("source_url"),
              "client sends only the v2 contract");
        check(q.get_header_value("Host").starts_with("unresolvable.invalid"),
              "IP pinning preserves the Host header");
        r.status = 201;
        r.set_content(Json{{"id", id}, {"lease_seconds", 120}, {"manifest", manifest(4)}}.dump(),
                      "application/json");
    });
    server.http.Put(base + "/lease",
                    [](const auto &, auto &r) { r.set_content("{}", "application/json"); });
    server.http.Delete(base, [&](const auto &, auto &r) {
        ++closed;
        r.status = 204;
    });
    server.http.Get(base + "/media/init", [&](const auto &, auto &r) {
        reading.set_value();
        r.set_content_provider(1, "video/x-matroska", [&](size_t, size_t, httplib::DataSink &sink) {
            while (!release)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            return sink.write("x", 1);
        });
    });
    server.start();
    auto request = std::make_unique<OpenSession>(
        config(server.port, token.path), "local",
        OpenRequest{Input{{"https://cdn.example/video"}, Json::object()}, std::nullopt, {}});
    try {
        check(reading.get_future().wait_for(std::chrono::seconds(3)) == std::future_status::ready,
              "session creation runs asynchronously");
        const auto start = Clock::now();
        request.reset();
        check(Clock::now() - start < std::chrono::seconds(2),
              "cancel interrupts a blocked initialization read");
        check(closed == 1, "RAII closes a session created before activation is canceled");
    } catch (...) {
        release = true;
        throw;
    }
    release = true;
}
void browser_open_timeout() {
    TempFile token(random_id() + random_id());
    Server server;
    server.http.Post("/api/v2/browser/sessions", [](const auto &, auto &response) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5200));
        response.status = 201;
        response.set_content("{\"created\":true}", "application/json");
    });
    server.start();
    auto settings = config(server.port, token.path);
    settings.request_timeout_seconds = 10;
    const auto result = ApiClient(settings).request("POST", "/api/v2/browser/sessions", Json::object());
    check(result.at("created") == true, "browser session creation uses the configured source probe timeout");
}
void redaction() {
    const auto directory = std::filesystem::temp_directory_path() / ("modeld-log-" + random_id());
    const char *prior = std::getenv("XDG_STATE_HOME");
    const std::string saved = prior ? prior : "";
    setenv("XDG_STATE_HOME", directory.c_str(), 1);
    log_event("test", {{"token", "private_token"},
                       {"message", "Cookie: private_cookie"},
                       {"error", "https://user:private_password@cdn.example/"
                                 "private_path?key=private_query Bearer private_bearer"}});
    std::ifstream input(client_log_path());
    const std::string data{std::istreambuf_iterator<char>(input), {}};
    check(!data.empty(), "diagnostics are persisted");
    for (const auto *value : {"private_token", "private_cookie", "private_password", "private_path",
                              "private_query", "private_bearer"})
        check(data.find(value) == std::string::npos,
              "diagnostics redact credentials and media paths");
    struct stat info{};
    check(::stat(client_log_path().c_str(), &info) == 0 && (info.st_mode & 0777) == 0600,
          "diagnostics are private");
    if (prior)
        setenv("XDG_STATE_HOME", saved.c_str(), 1);
    else
        unsetenv("XDG_STATE_HOME");
    std::filesystem::remove_all(directory);
}
} // namespace
int main() {
    try {
        protocol();
        range();
        cache();
        canceled_open();
        browser_open_timeout();
        redaction();
        std::cout << "v2 contract, source ranges, cache and cancellation passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
