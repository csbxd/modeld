#include "modeld/common.hpp"
#include <fstream>
#include <iostream>
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
            upstream.Get(R"(/test/segments/([0-9]+)\.ts)", [](const auto &req, auto &res) {
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
                check(proxy.buffered_ahead(0) >= 2, "proxy tracks completed video");
                auto seek = client.Get(base + "segments/12.ts");
                check(seek && seek->body == "segment-12", "proxy cancels stale prefetch on seek");
                auto invalid = client.Get(base + "segments/99.ts");
                check(invalid && invalid->status == 404, "proxy rejects out-of-range segments");
            }
            upstream.stop();
            listener.join();
        }
        check(model_catalog()["data"][0]["precision"] == "fp32", "model precision advertised");
        std::cout << "Protocol, timeline, capability isolation and range serving passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
    return 0;
}
