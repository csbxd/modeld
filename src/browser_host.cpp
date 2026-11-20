#include "modeld/api.hpp"
#include <algorithm>
#include <condition_variable>
#include <csignal>
#include <deque>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <openssl/evp.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace {
using namespace modeld;
std::filesystem::path directory() {
    const char *xdg = std::getenv("XDG_CONFIG_HOME"), *home = std::getenv("HOME");
    return std::filesystem::path(xdg ? xdg : std::string(home ? home : "") + "/.config") / "modeld";
}
std::filesystem::path configuration() {
    const char *override = std::getenv("MODELD_BROWSER_CONFIG");
    return override ? std::filesystem::path(override) : directory() / "browser.json";
}
ClientConfig load() {
    return std::filesystem::exists(configuration()) ? ClientConfig::load(configuration())
                                                    : ClientConfig::load();
}
void private_file(const std::filesystem::path &path, const std::string &value) {
    std::filesystem::create_directories(path.parent_path());
    ::chmod(path.parent_path().c_str(), 0700);
    const auto temporary = path.string() + "." + random_id();
    {
        std::ofstream file(temporary, std::ios::binary);
        if (!file)
            throw Error(500, "Cannot write browser configuration");
        ::chmod(temporary.c_str(), 0600);
        file << value;
        if (!file)
            throw Error(500, "Cannot save browser configuration");
    }
    std::filesystem::rename(temporary, path);
}
Json public_configuration() {
    try {
        const auto c = load();
        return {{"endpoint", c.endpoint},
                {"connect_address", c.connect_address},
                {"configured", read_token(c.token_file).size() >= 32}};
    } catch (...) {
        return {{"endpoint", "http://s8030.netbird.cloud:8765"},
                {"connect_address", ""},
                {"configured", false}};
    }
}
std::vector<std::string> sources(const Json &value) {
    auto urls = value.get<std::vector<std::string>>();
    if (urls.empty() || urls.size() > 9)
        throw Error(422, "A browser input requires one to nine URLs");
    for (const auto &url : urls) {
        if (!url.starts_with("https://") && !url.starts_with("http://"))
            throw Error(422, "Media must use HTTP(S)");
        httplib::Client parsed(url);
        const auto host = parsed.host();
        if (!host.ends_with(".bilivideo.com") && !host.ends_with(".bilivideo.cn"))
            throw Error(422, "Browser input must be a Bilibili media host");
    }
    return urls;
}
struct Owned {
    ClientConfig config;
    Manifest manifest;
};
struct Host {
    std::mutex output_mutex, sessions_mutex, queue_mutex;
    std::condition_variable_any ready;
    std::deque<Json> queue;
    std::map<std::string, Owned> sessions;
    bool closing = false;
    std::vector<std::jthread> workers;
    void send(const Json &value) {
        const auto text = value.dump(-1, ' ', false, Json::error_handler_t::replace);
        if (text.size() > (1U << 20))
            throw Error(500, "Native response is too large");
        const uint32_t size = static_cast<uint32_t>(text.size());
        unsigned char header[]{
            static_cast<unsigned char>(size), static_cast<unsigned char>(size >> 8),
            static_cast<unsigned char>(size >> 16), static_cast<unsigned char>(size >> 24)};
        std::lock_guard lock(output_mutex);
        std::cout.write(reinterpret_cast<const char *>(header), 4);
        std::cout.write(text.data(), text.size());
        std::cout.flush();
        if (!std::cout)
            throw Error(499, "Browser channel closed", "canceled");
    }
    void binary(int64_t id, const std::string &bytes) {
        constexpr size_t chunk_size = 192U << 10;
        size_t part = 0;
        for (size_t offset = 0; offset < bytes.size(); offset += chunk_size, ++part) {
            const auto length = std::min(chunk_size, bytes.size() - offset);
            std::string encoded(4 * ((length + 2) / 3), '\0');
            EVP_EncodeBlock(reinterpret_cast<unsigned char *>(encoded.data()),
                            reinterpret_cast<const unsigned char *>(bytes.data() + offset),
                            static_cast<int>(length));
            send({{"id", id},
                  {"ok", true},
                  {"part", part},
                  {"done", offset + length == bytes.size()},
                  {"bytes", bytes.size()},
                  {"data", encoded}});
        }
        if (bytes.empty())
            send({{"id", id},
                  {"ok", true},
                  {"part", 0},
                  {"done", true},
                  {"bytes", 0},
                  {"data", ""}});
    }
    Owned owned(const std::string &id) {
        std::lock_guard lock(sessions_mutex);
        const auto found = sessions.find(id);
        if (found == sessions.end())
            throw Error(404, "Browser session is closed", "session_closed");
        return found->second;
    }
    Json process(const Json &message, std::stop_token stop) {
        const auto action = message.at("action").get<std::string>();
        if (action == "config_get")
            return public_configuration();
        if (action == "config_save") {
            const auto &input = message.at("value");
            ClientConfig previous;
            try {
                previous = load();
            } catch (...) {
            }
            const auto token = input.value("token", std::string{});
            auto token_file = previous.token_file;
            if (!token.empty()) {
                if (token.size() < 32 || token.find_first_of("\r\n") != std::string::npos)
                    throw Error(422, "API token must contain at least 32 characters");
                token_file = configuration().string() + ".token";
            }
            const auto data = Json{{"endpoint", input.at("endpoint")},
                                   {"connect_address", input.value("connect_address", "")},
                                   {"token_file", token_file},
                                   {"source_bind", "auto"}};
            const auto c = ClientConfig::parse(data);
            if (token.empty())
                read_token(c.token_file);
            else
                private_file(token_file, token + "\n");
            private_file(configuration(), data.dump(2) + "\n");
            return public_configuration();
        }
        if (action == "health") {
            ApiClient api(load());
            auto result = api.request("GET", "/healthz", nullptr, stop);
            result["browser"] = api.request("GET", "/api/v2/browser/capabilities", nullptr, stop);
            return result;
        }
        if (action == "open") {
            const auto c = load();
            ApiClient api(c);
            const auto video_codec = message.value("video_codec", std::string("h264"));
            if (video_codec != "h264" && video_codec != "av1")
                throw Error(422, "Unsupported browser output codec");
            const auto user_agent = message.value("user_agent", std::string("Mozilla/5.0"));
            if (user_agent.size() > 1024 || user_agent.find_first_of("\r\n") != std::string::npos)
                throw Error(422, "Invalid browser user agent");
            const Json headers = {{"Referer", "https://www.bilibili.com/"},
                                  {"User-Agent", user_agent}};
            Json body = {{"model", model_id},
                         {"video", {{"urls", sources(message.at("video"))}, {"headers", headers}}}};
            if (message.contains("audio") && !message["audio"].is_null())
                body["audio"] = {{"urls", sources(message["audio"])}, {"headers", headers}};
            if (message.contains("qp"))
                body["qp"] = message.at("qp");
            else if (video_codec == "h264")
                body["qp"] = 18;
            if (video_codec == "av1") {
                const auto capabilities =
                    api.request("GET", "/api/v2/browser/capabilities", nullptr, stop);
                const auto codecs = capabilities.value("video_codecs", Json::array());
                if (std::find(codecs.begin(), codecs.end(), "av1") == codecs.end())
                    throw Error(422, "远端 modeld 未提供 AV1，请升级服务端或选择 H.264。",
                                "unsupported_media");
                body["video_codec"] = video_codec;
            }
            auto created = api.request("POST", "/api/v2/browser/sessions", body, stop);
            auto normal = created;
            normal.erase("mime_type");
            const auto info = SessionInfo::parse(normal);
            {
                std::lock_guard lock(sessions_mutex);
                sessions.emplace(info.id, Owned{c, info.manifest});
            }
            return created;
        }
        const auto session = message.at("session").get<std::string>();
        auto current = owned(session);
        ApiClient api(current.config);
        const auto base = std::string(api_prefix) + "/sessions/" + session;
        if (action == "lease")
            return api.request("PUT", base + "/lease", {{"paused", message.at("paused")}}, stop);
        if (action == "close") {
            api.request("DELETE", base, nullptr, stop);
            std::lock_guard lock(sessions_mutex);
            sessions.erase(session);
            return nullptr;
        }
        if (action == "init" || action == "segment") {
            const auto path = action == "init"
                                  ? current.manifest.init
                                  : current.manifest.segment(message.at("index").get<int64_t>());
            auto client = api.connection();
            client->set_read_timeout(90);
            client->set_payload_max_length(64ULL << 20);
            std::stop_callback cancel(stop, [&] { client->stop(); });
            auto response = client->Get(path);
            if (!response)
                throw Error(503, "Media connection failed", "transfer_failed");
            if (response->status != 200) {
                const auto error = Json::parse(response->body);
                throw Error(response->status, error.at("error").at("message"),
                            error.at("error").at("code"));
            }
            binary(message.at("id").get<int64_t>(), response->body);
            return Json::value_t::discarded;
        }
        throw Error(422, "Unknown native action");
    }
    Host() {
        for (int i = 0; i < 6; ++i)
            workers.emplace_back([this](std::stop_token stop) {
                while (!stop.stop_requested()) {
                    Json request;
                    {
                        std::unique_lock lock(queue_mutex);
                        ready.wait(lock, stop, [&] { return closing || !queue.empty(); });
                        if (closing || stop.stop_requested())
                            return;
                        request = std::move(queue.front());
                        queue.pop_front();
                        ready.notify_all();
                    }
                    const auto id = request.value("id", int64_t{0});
                    try {
                        const auto result = process(request, stop);
                        if (!result.is_discarded())
                            send({{"id", id}, {"ok", true}, {"result", result}});
                    } catch (const Error &e) {
                        try {
                            send({{"id", id},
                                  {"ok", false},
                                  {"error",
                                   {{"code", e.code},
                                    {"message", e.what()},
                                    {"retryable", e.retryable}}}});
                        } catch (...) {
                        }
                    } catch (const std::exception &e) {
                        try {
                            send({{"id", id},
                                  {"ok", false},
                                  {"error",
                                   {{"code", "native_error"},
                                    {"message", e.what()},
                                    {"retryable", false}}}});
                        } catch (...) {
                        }
                    }
                }
            });
    }
    void run() {
        for (;;) {
            unsigned char bytes[4];
            if (!std::cin.read(reinterpret_cast<char *>(bytes), 4))
                break;
            const uint32_t size = uint32_t(bytes[0]) | (uint32_t(bytes[1]) << 8) |
                                  (uint32_t(bytes[2]) << 16) | (uint32_t(bytes[3]) << 24);
            if (!size || size > (1U << 20))
                break;
            std::string data(size, '\0');
            if (!std::cin.read(data.data(), size))
                break;
            auto message = Json::parse(data, nullptr, false);
            if (!message.is_object() || !message.contains("id") ||
                !message["id"].is_number_integer())
                break;
            std::unique_lock lock(queue_mutex);
            ready.wait(lock, [&] { return queue.size() < 16; });
            queue.push_back(std::move(message));
            ready.notify_all();
        }
    }
    ~Host() {
        {
            std::lock_guard lock(queue_mutex);
            closing = true;
            queue.clear();
        }
        for (auto &worker : workers)
            worker.request_stop();
        ready.notify_all();
        workers.clear();
        for (const auto &[id, current] : sessions)
            try {
                ApiClient(current.config)
                    .request("DELETE", std::string(api_prefix) + "/sessions/" + id);
            } catch (...) {
            }
    }
};
} // namespace
int main(int argc, char **argv) {
    if (argc == 2 && std::string(argv[1]) == "--version") {
        std::cout << MODELD_VERSION << '\n';
        return 0;
    }
    std::signal(SIGPIPE, SIG_IGN);
    try {
        Host host;
        host.run();
        return 0;
    } catch (...) {
        return 1;
    }
}
