#include "modeld/common.hpp"
#include <fstream>
#include <iostream>
#include <sys/stat.h>
#include <thread>
static void check(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}
int main() {
    using namespace modeld;
    try {
        auto id = random_id();
        check(id.size() == 32 && id != random_id(), "unpredictable capability IDs");
        check(secure_equal("same", "same") && !secure_equal("same", "other"), "token comparison");
        auto p = playlist(4.01, 60, 2);
        check(p.find("#EXTINF:0.016667,") != std::string::npos, "fractional final frame duration");
        check(p.find("segments/2.ts") != std::string::npos &&
                  p.find("segments/3.ts") == std::string::npos,
              "bounded segment timeline");
        bool rejected = false;
        try {
            playlist(-1, 60, 2);
        } catch (const Error &) {
            rejected = true;
        }
        check(rejected, "invalid duration rejected");
        {
            const std::string video_a = "https://cdn.example/hevc.m4s?key=a,b;part=1";
            const std::string video_b = "https://cdn.example/avc.m4s?key=other";
            auto quoted = [](const std::string &value) {
                return "%" + std::to_string(value.size()) + "%" + value;
            };
            const auto dash =
                "edl://!new_stream;!no_clip;!delay_open,media_type=video,codec=null;"
                "!track_meta,title=%12%1080P 高清,program_id=0;" +
                quoted(video_a) +
                ",length=1097;!new_stream;!no_clip;!delay_open,media_type=video,codec=h264;"
                "!track_meta,title=%12%1080P 高清,program_id=1;" +
                quoted(video_b) +
                ",length=1097;!new_stream;!delay_open,media_type=audio;"
                "https://cdn.example/audio.m4s";
            check(resolved_video_url(dash, {{"program-id", 0}}) == video_a,
                  "DASH selected HEVC program and length-escaped URL");
            check(resolved_video_url(dash, {{"program-id", 1}}) == video_b,
                  "DASH selected AVC program with the same title");
            check(resolved_video_url(video_a, Json::object()) == video_a,
                  "direct media URLs are preserved");
            for (const auto &unsupported :
                 {std::string("edl://%999%short"),
                  std::string("edl://https://cdn.example/part1;https://cdn.example/part2"),
                  std::string("edl://!mp4_dash;https://cdn.example/init.mp4"),
                  std::string("edl://https://cdn.example/video,start=10,length=20")}) {
                bool denied = false;
                try {
                    resolved_video_url(unsupported, Json::object());
                } catch (const Error &) {
                    denied = true;
                }
                check(denied, "malformed or spliced timelines cannot select the wrong video");
            }
        }
        {
            const std::string original =
                "https://peer.mcdn.bilivideo.cn:8082/upgcxcode/a/42-1-100024.m4s?peer";
            const std::string backup =
                "https://upos-sz-mirrorhw.bilivideo.com/upgcxcode/a/42-1-100024.m4s?cdn";
            const auto sources = bilibili_video_sources(
                original,
                {{"dash",
                  {{"video",
                    Json::array(
                        {{{"baseUrl", original},
                          {"backupUrl",
                           Json::array({backup, backup,
                                        "https://attacker.example/upgcxcode/a/42-1-100024.m4s"})}},
                         {{"baseUrl",
                           "https://upos-sz-mirrorhw.bilivideo.com/upgcxcode/a/42-1-30064.m4s"}},
                         {{"baseUrl", "https://upos-sz-mirrorhw.bilivideo.com/upgcxcode/a/"
                                      "42-1-100023.m4s"}}})}}}});
            check(sources == std::vector<std::string>({backup, original}),
                  "CDN fallback preserves the exact encoding, deduplicates and rejects foreign "
                  "hosts");
            check(bilibili_video_sources(original, Json::object()) ==
                      std::vector<std::string>({original}),
                  "missing backup metadata preserves the original selection");
        }
        auto file = std::filesystem::temp_directory_path() / ("modeld-test-" + random_id());
        {
            std::ofstream f(file);
            f << "0123456789";
        }
        {
            SourceServer source(file, "127.0.0.1");
            auto url = source.url();
            auto split = url.find('/', 7);
            httplib::Client client(url.substr(0, split));
            auto path = url.substr(split);
            auto r = client.Get(path, httplib::Headers{{"Range", "bytes=2-5"}});
            check(r && r->status == 206 && r->body == "2345", "source byte ranges");
            auto denied = client.Get("/source/not-the-capability");
            check(denied && denied->status == 404, "unshared source unavailable");
        }
        std::filesystem::remove(file);
        {
            httplib::Server upstream;
            upstream.Get("/test/index.m3u8", [](const auto &, auto &res) {
                res.set_content(
                    "#EXTM3U\n#EXT-X-TARGETDURATION:2\n#EXTINF:2,\nsegments/0.ts\n#EXT-X-ENDLIST\n",
                    "application/vnd.apple.mpegurl");
            });
            std::atomic<int> attempts{0};
            upstream.Get(R"(/test/segments/([0-9]+)\.ts)", [&](const auto &req, auto &res) {
                if (req.matches[1] == "0" && ++attempts <= 2) {
                    res.status = 503;
                    res.set_content("temporary source failure", "text/plain");
                    return;
                }
                if (req.matches[1] == "9") {
                    res.status = 404;
                    return;
                }
                res.set_content("segment-" + req.matches[1].str(), "video/MP2T");
            });
            int port = upstream.bind_to_any_port("127.0.0.1");
            std::thread listener([&] { upstream.listen_after_bind(); });
            {
                MediaProxy proxy("http://127.0.0.1:" + std::to_string(port) + "/test/index.m3u8",
                                 32);
                auto url = proxy.url();
                auto split = url.find('/', 7);
                httplib::Client client(url.substr(0, split));
                auto base = url.substr(split);
                base.erase(base.rfind('/') + 1);
                auto first = client.Get(base + "segments/0.ts");
                check(first && first->body == "segment-0", "proxy preserves segment bytes");
                check(attempts == 3 && proxy.failure().empty(),
                      "proxy recovers repeated transient HTTP errors without aborting playback");
                check(proxy.buffered_ahead(0) >= 2, "proxy tracks completed video");
                auto seek = client.Get(base + "segments/12.ts");
                check(seek && seek->body == "segment-12", "proxy cancels stale prefetch on seek");
                auto invalid = client.Get(base + "segments/99.ts");
                check(invalid && invalid->status == 404, "proxy rejects out-of-range segments");
                auto missing = client.Get(base + "segments/9.ts");
                check(missing && missing->status == 503 &&
                          proxy.failure().find("404") != std::string::npos,
                      "terminal transfer errors retain the upstream HTTP status");
            }
            upstream.stop();
            listener.join();
        }
        check(model_catalog()["data"][0]["precision"] == "fp32", "model precision advertised");
        {
            const char *prior = std::getenv("XDG_STATE_HOME");
            const std::string saved = prior ? prior : "";
            auto directory = std::filesystem::temp_directory_path() / ("modeld-log-" + random_id());
            setenv("XDG_STATE_HOME", directory.c_str(), 1);
            log_event(
                "test",
                {{"token", "secret_token"},
                 {"source_headers", {{"Cookie", "secret_cookie"}}},
                 {"error",
                  "failed https://user:secret_password@cdn.example/private_media?key=secret_query "
                  "Bearer secret_bearer"},
                 {"ipv6", "https://[::1]/private_media?key=secret_ipv6"},
                 {"message", "Cookie: session=secret_cookie"}});
            std::ifstream log(client_log_path());
            const std::string text{std::istreambuf_iterator<char>(log), {}};
            check(!text.empty() && text.find("cdn.example/[redacted]") != std::string::npos,
                  "persistent diagnostics keep origin hosts");
            for (const auto *secret :
                 {"secret_token", "secret_cookie", "secret_password", "secret_query",
                  "secret_bearer", "secret_ipv6", "private_media"})
                check(text.find(secret) == std::string::npos,
                      "diagnostics redact credentials and URL paths");
            struct stat status{};
            check(::stat(client_log_path().c_str(), &status) == 0 &&
                      (status.st_mode & 0777) == 0600,
                  "private diagnostic file permissions");
            if (prior)
                setenv("XDG_STATE_HOME", saved.c_str(), 1);
            else
                unsetenv("XDG_STATE_HOME");
            std::filesystem::remove_all(directory);
        }
        std::cout << "Protocol, timeline, capability isolation and range serving passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
    return 0;
}
