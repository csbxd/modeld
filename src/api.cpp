#include "modeld/api.hpp"
namespace modeld {
std::unique_ptr<httplib::Client> ApiClient::connection() const {
    auto client = std::make_unique<httplib::Client>(config_.endpoint);
    config_.configure(*client);
    client->set_connection_timeout(5);
    return client;
}
Json ApiClient::request(const std::string &method, const std::string &path, const Json &body,
                        std::stop_token stop) const {
    if (method != "GET" && method != "POST" && method != "PUT" && method != "DELETE")
        throw Error(422, "Unsupported API method");
    auto client = connection();
    const bool opening = method == "POST" &&
                         (path == std::string(api_prefix) + "/sessions" ||
                          path == std::string(api_prefix) + "/browser/sessions");
    const int timeout = opening ? config_.request_timeout_seconds : method == "DELETE" ? 3 : 5;
    client->set_connection_timeout(std::min(5, timeout));
    client->set_read_timeout(timeout);
    client->set_max_timeout((timeout + 2) * 1000);
    client->set_payload_max_length(4 * 1024 * 1024);
    std::stop_callback cancel(stop, [&] { client->stop(); });
    if (stop.stop_requested())
        throw Error(499, "Request canceled", "canceled");
    httplib::Headers headers;
    if (path != "/healthz")
        headers.emplace("Authorization", "Bearer " + read_token(config_.token_file));
    const auto started = Clock::now();
    const bool lease = path.ends_with("/lease");
    if (!lease)
        log_event("api.request", {{"method", method}, {"path", log_url(config_.endpoint + path)}});
    auto response = method == "POST"  ? client->Post(path, headers, body.dump(), "application/json")
                    : method == "PUT" ? client->Put(path, headers, body.dump(), "application/json")
                    : method == "DELETE" ? client->Delete(path, headers)
                                         : client->Get(path, headers);
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count();
    if (!response) {
        if (stop.stop_requested())
            throw Error(499, "Request canceled", "canceled");
        log_event("api.failed",
                  {{"elapsed_ms", elapsed}, {"error", httplib::to_string(response.error())}});
        const bool timed_out =
            response.error() == httplib::Error::Timeout || elapsed >= timeout * 1000;
        throw Error(timed_out ? 504 : 503,
                    timed_out ? "API request timed out" : "API connection failed",
                    timed_out ? "api_timeout" : "api_unavailable");
    }
    if (!lease || response->status >= 400)
        log_event("api.response", {{"status", response->status}, {"elapsed_ms", elapsed}});
    if (response->status == 204)
        return nullptr;
    Json result;
    try {
        result = Json::parse(response->body);
    } catch (...) {
        throw Error(502, "API returned an invalid JSON response", "invalid_response");
    }
    if (response->status >= 400) {
        if (!result.is_object() || !result.contains("error") || !result["error"].is_object() ||
            !result["error"].contains("code") || !result["error"]["code"].is_string() ||
            !result["error"].contains("message") || !result["error"]["message"].is_string() ||
            !result["error"].contains("retryable") || !result["error"]["retryable"].is_boolean())
            throw Error(502, "API returned an invalid error response", "invalid_response");
        const auto &error = result.at("error");
        Error failure(response->status, error.at("message"), error.at("code"));
        failure.retryable = error.at("retryable");
        throw failure;
    }
    return result;
}
} // namespace modeld
