#include "modeld/common.hpp"
#include <csignal>
#include <deque>
#include <fstream>
#include <iostream>
#include <thread>
static volatile std::sig_atomic_t stopped = 0;
int main(int argc, char **argv) {
    using namespace modeld;
    try {
        if (argc < 2)
            throw Error(400, "Usage: modelctl models | health | source FILE | session JSON | "
                             "delete ID | logs [N] | log-path");
        std::string cmd = argv[1];
        if (cmd == "log-path") {
            std::cout << client_log_path().string() << '\n';
            return 0;
        }
        if (cmd == "logs") {
            size_t count = argc == 3 ? std::stoul(argv[2]) : 100;
            if (count < 1 || count > 5000)
                throw Error(400, "Log line count must be 1..5000");
            std::ifstream input(client_log_path());
            if (!input)
                throw Error(404, "No client log yet: " + client_log_path().string());
            std::deque<std::string> lines;
            for (std::string line; std::getline(input, line);) {
                lines.push_back(std::move(line));
                if (lines.size() > count)
                    lines.pop_front();
            }
            for (const auto &line : lines)
                std::cout << line << '\n';
            return 0;
        }
        auto c = client_config();
        if (cmd == "models")
            std::cout << request_json(c, "GET", "/v1/models").dump(2) << '\n';
        else if (cmd == "health") {
            std::cout << request_json(c, "GET", "/health").dump(2) << '\n';
        } else if (cmd == "media-url" && argc == 3)
            std::cout << media_url(argv[2], c) << '\n';
        else if (cmd == "session" && argc == 3)
            std::cout << request_json(c, "POST", "/v1/sessions", Json::parse(argv[2])).dump()
                      << '\n';
        else if (cmd == "delete" && argc == 3)
            std::cout << request_json(c, "DELETE", "/v1/sessions/" + std::string(argv[2])).dump()
                      << '\n';
        else if (cmd == "source" && argc == 3) {
            std::signal(SIGINT, [](int) { stopped = 1; });
            std::signal(SIGTERM, [](int) { stopped = 1; });
            auto a = c.value("callback_address", std::string("auto"));
            SourceServer server(argv[2], a == "auto" ? netbird_address() : a);
            std::cout << Json({{"source_url", server.url()}}).dump() << std::endl;
            while (!stopped)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
        } else
            throw Error(400, "Unknown command or arguments");
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "modelctl: " << e.what() << '\n';
        return 1;
    }
}
