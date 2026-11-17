// Exercise native probing without initializing TensorRT or rendering any frames.
#include "modeld/common.hpp"
#include "modeld/media.hpp"
#include <iostream>
#include <thread>

int main(int argc, char **argv) {
    using namespace modeld;
    if (argc < 3) {
        std::cerr << "Usage: modeld-source-probe URL TIMEOUT_SECONDS [CANCEL_AFTER_MS]\n";
        return 2;
    }
    auto canceled = std::make_shared<std::atomic<bool>>(false);
    std::jthread cancel;
    if (argc == 4)
        cancel = std::jthread([canceled, ms = std::stoi(argv[3])](std::stop_token stop) {
            const auto end = Clock::now() + std::chrono::milliseconds(ms);
            while (!stop.stop_requested() && Clock::now() < end)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            if (!stop.stop_requested())
                canceled->store(true);
        });
    const auto started = Clock::now();
    try {
        Video video({argv[1]}, "", 2, 18, "hevc_nvenc", {}, canceled, std::stoi(argv[2]));
        const auto info = video.info();
        std::cout << Json{{"width", info.width},
                          {"height", info.height},
                          {"source_fps", info.source_fps},
                          {"duration", info.duration},
                          {"seconds",
                           std::chrono::duration<double>(Clock::now() - started).count()}}
                         .dump()
                  << '\n';
    } catch (const Error &e) {
        std::cout << Json{{"status", e.status},
                          {"error", e.what()},
                          {"seconds",
                           std::chrono::duration<double>(Clock::now() - started).count()}}
                         .dump()
                  << '\n';
        return 1;
    }
}
