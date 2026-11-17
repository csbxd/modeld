#include "modeld/common.hpp"
#include <csignal>
#include <iostream>
#include <thread>
static volatile std::sig_atomic_t stopped = 0;
int main(int argc, char **argv) {
    using namespace modeld;
    try {
        auto c = client_config();
        if (argc < 2)
            throw Error(400,
                        "Usage: modelctl models | health | source FILE | session JSON | delete ID");
        std::string cmd = argv[1];
        if (cmd == "models")
            std::cout << request_json(c, "GET", "/v1/models").dump(2) << '\n';
        else if (cmd == "health") {
            httplib::Client client(c.at("api").get<std::string>());
            auto r = client.Get("/health");
            if (!r)
                throw Error(503, "API unavailable");
            std::cout << r->body << '\n';
            return r->status == 200 ? 0 : 1;
        } else if (cmd == "media-url" && argc == 3)
            std::cout << media_url(argv[2]) << '\n';
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
