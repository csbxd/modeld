#include "modeld/input.hpp"
#include <fcntl.h>
#include <httplib.h>
#include <thread>
#include <unistd.h>
namespace modeld {
struct SourceServer::Impl {
    httplib::Server server;
    std::thread thread;
    int fd = -1;
    std::string address, token;
    int port = 0;
    ~Impl() {
        server.stop();
        if (thread.joinable())
            thread.join();
        if (fd >= 0)
            ::close(fd);
    }
};
SourceServer::SourceServer(const std::filesystem::path &input, const std::string &bind)
    : impl(std::make_unique<Impl>()) {
    auto path = std::filesystem::canonical(input);
    if (!std::filesystem::is_regular_file(path))
        throw Error(422, "Only regular media files can be shared");
    impl->fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (impl->fd < 0)
        throw Error(500, "Cannot open selected media file");
    impl->address = bind;
    impl->token = random_id();
    auto size = std::filesystem::file_size(path);
    int fd = impl->fd;
    impl->server.Get(
        "/source/" + impl->token, [size, fd](const httplib::Request &, httplib::Response &res) {
            res.set_header("Cache-Control", "private, no-store");
            res.set_header("Accept-Ranges", "bytes");
            res.set_content_provider(size, "application/octet-stream",
                                     [fd](size_t offset, size_t length, httplib::DataSink &sink) {
                                         char buffer[65536];
                                         auto n = ::pread(fd, buffer,
                                                          std::min(length, sizeof buffer), offset);
                                         return n > 0 && sink.write(buffer, static_cast<size_t>(n));
                                     });
        });
    impl->server.new_task_queue = [] { return new httplib::ThreadPool(4); };
    impl->server.set_read_timeout(10);
    impl->server.set_write_timeout(30);
    impl->port = impl->server.bind_to_any_port(bind);
    if (impl->port < 0)
        throw Error(503, "Cannot bind source server to NetBird address");
    impl->thread = std::thread([this] { impl->server.listen_after_bind(); });
}
SourceServer::~SourceServer() = default;
std::string SourceServer::url() const {
    const auto host =
        impl->address.find(':') == std::string::npos ? impl->address : "[" + impl->address + "]";
    return "http://" + host + ":" + std::to_string(impl->port) + "/source/" + impl->token;
}
} // namespace modeld
