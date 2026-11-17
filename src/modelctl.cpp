#include "modeld/session.hpp"
#include <csignal>
#include <deque>
#include <fstream>
#include <iostream>
#include <thread>
namespace {
volatile std::sig_atomic_t stopped = 0;
void print_logs(int count) {
    if (count < 1 || count > 5000)
        throw modeld::Error(422, "Log count must be 1..5000");
    std::ifstream file(modeld::client_log_path());
    if (!file)
        throw modeld::Error(404, "No client log yet");
    std::deque<std::string> lines;
    for (std::string line; std::getline(file, line);) {
        lines.push_back(std::move(line));
        if (lines.size() > static_cast<size_t>(count))
            lines.pop_front();
    }
    for (const auto &line : lines)
        std::cout << line << '\n';
}
} // namespace
int main(int argc, char **argv) {
    using namespace modeld;
    try {
        if (argc == 2 && std::string(argv[1]) == "--version") {
            std::cout << MODELD_VERSION << '\n';
            return 0;
        }
        int first = 1;
        std::filesystem::path config;
        if (argc > 3 && std::string(argv[first]) == "--config") {
            config = argv[++first];
            ++first;
        }
        if (first >= argc)
            throw Error(422,
                        "Usage: modelctl [--config FILE] health|models|status|sessions|inspect "
                        "ID|open FILE|lease ID playing|paused|close ID|source "
                        "FILE|check-config|logs [N]|log-path");
        const std::string command = argv[first++];
        const int remaining = argc - first;
        if (command == "log-path" && remaining == 0) {
            std::cout << client_log_path() << '\n';
            return 0;
        }
        if (command == "logs" && remaining <= 1) {
            print_logs(remaining ? std::stoi(argv[first]) : 100);
            return 0;
        }
        const auto settings = ClientConfig::load(config);
        const ApiClient api(settings);
        Json result;
        const std::string sessions = std::string(api_prefix) + "/sessions";
        if (command == "check-config" && remaining == 0) {
            read_token(settings.token_file);
            result = {{"status", "valid"}, {"protocol", 2}};
        } else if (command == "health" && remaining == 0)
            result = api.request("GET", "/healthz");
        else if ((command == "models" || command == "status") && remaining == 0)
            result = api.request("GET", std::string(api_prefix) + "/" + command);
        else if (command == "sessions" && remaining == 0)
            result = api.request("GET", sessions);
        else if (command == "inspect" && remaining == 1)
            result = api.request("GET", sessions + "/" + argv[first]);
        else if (command == "close" && remaining == 1)
            result = api.request("DELETE", sessions + "/" + argv[first]);
        else if (command == "open" && remaining == 1) {
            Json request =
                std::string(argv[first]) == "-" ? Json::parse(std::cin) : read_json(argv[first]);
            result = api.request("POST", sessions, request);
        } else if (command == "lease" && remaining == 2) {
            const std::string state = argv[first + 1];
            if (state != "playing" && state != "paused")
                throw Error(422, "Lease state must be playing or paused");
            result = api.request("PUT", sessions + "/" + argv[first] + "/lease",
                                 {{"paused", state == "paused"}});
        } else if (command == "source" && remaining == 1) {
            std::signal(SIGINT, [](int) { stopped = 1; });
            std::signal(SIGTERM, [](int) { stopped = 1; });
            SourceServer source(argv[first], settings.source_bind == "auto" ? netbird_address()
                                                                            : settings.source_bind);
            std::cout << Input{{source.url()}, Json::object()}.json().dump() << std::endl;
            while (!stopped)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            return 0;
        } else
            throw Error(422, "Unknown command or arguments");
        std::cout << result.dump(2) << '\n';
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "modelctl: " << e.what() << '\n';
        return 1;
    }
}
