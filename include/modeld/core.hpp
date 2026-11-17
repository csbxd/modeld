#pragma once
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <utility>
namespace modeld {
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
struct Error : std::runtime_error {
    int status;
    std::string code;
    bool retryable;
    Error(int status, const std::string &message, std::string code = "client_error")
        : std::runtime_error(message), status(status), code(std::move(code)),
          retryable(status == 408 || status == 429 || status == 500 || status == 502 ||
                    status == 503 || status == 504) {}
};
Json read_json(const std::filesystem::path &path);
std::string read_token(const std::filesystem::path &path);
std::string random_id();
std::string netbird_address();
void require_keys(const Json &value, std::initializer_list<const char *> allowed);
std::filesystem::path client_log_path();
void log_event(const std::string &event, const Json &details = Json::object()) noexcept;
std::string log_url(const std::string &url);
Json log_tracks(const Json &tracks);
} // namespace modeld
