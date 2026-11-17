// Manual integration fixture: serve a seekable video, interrupt its active
// primary connection with POST /fail, and leave /backup healthy. Bind locally.
#include "modeld/common.hpp"
#include <atomic>
#include <fstream>
#include <iostream>
#include <thread>

int main(int argc, char **argv) {
    if (argc != 3) {
        std::cerr << "Usage: modeld-fault-source VIDEO PORT\n";
        return 2;
    }
    std::ifstream input(argv[1], std::ios::binary);
    if (!input)
        return 2;
    const std::string bytes{std::istreambuf_iterator<char>(input), {}};
    std::atomic<bool> fail{false};
    std::atomic<int> primary{0}, backup{0}, interrupted{0};
    httplib::Server server;
    server.Post("/fail", [&](const auto &, auto &res) {
        fail = true;
        res.set_content("armed", "text/plain");
    });
    server.Get("/stats", [&](const auto &, auto &res) {
        res.set_content(modeld::Json{{"primary", primary.load()},
                                     {"backup", backup.load()},
                                     {"interrupted", interrupted.load()}}
                            .dump(),
                        "application/json");
    });
    for (const bool primary_source : {true, false}) {
        server.Get(
            primary_source ? "/primary" : "/backup", [&, primary_source](const auto &, auto &res) {
                if (primary_source) {
                    ++primary;
                    if (fail) {
                        res.status = 503;
                        return;
                    }
                } else {
                    ++backup;
                }
                res.set_header("Accept-Ranges", "bytes");
                res.set_content_provider(
                    bytes.size(), "video/mp4",
                    [&, primary_source](size_t offset, size_t length, httplib::DataSink &sink) {
                        if (primary_source && fail) {
                            ++interrupted;
                            return false;
                        }
                        auto size = std::min(length, size_t{4096});
                        if (primary_source)
                            std::this_thread::sleep_for(std::chrono::milliseconds(10));
                        return sink.write(bytes.data() + offset, size);
                    });
            });
    }
    return server.listen("127.0.0.1", std::stoi(argv[2])) ? 0 : 1;
}
