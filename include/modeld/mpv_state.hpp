#pragma once
#include "modeld/protocol.hpp"
#include <map>
#include <memory>
#include <mpv/client.h>
namespace modeld {
class Mpv {
    mpv_handle *handle_;

  public:
    explicit Mpv(mpv_handle *handle) : handle_(handle) {}
    std::string text(const char *name) const;
    Json get(const char *name) const;
    void set(const std::string &name, const Json &value) const;
    void command(const Json &args, bool async = false) const;
    void osd(const std::string &text, int milliseconds = 4000) const;
    double position() const;
    bool paused() const;
};
struct Playback {
    struct Subtitle {
        int fd = -1;
        std::string url;
        ~Subtitle();
    };
    std::string path, opened, webpage, title, video, audio, subtitle, secondary_subtitle, edition,
        agent, referrer;
    Json tracks, headers, chapters;
    std::map<int, std::shared_ptr<Subtitle>> subtitles;
    static Playback capture(const Mpv &mpv);
    Json track(const std::string &kind, const std::string &id) const;
    Json request_headers() const;
    std::string mapped(const Manifest &manifest, const std::string &kind,
                       const std::string &id) const;
    void restore(const Mpv &mpv, const Manifest *processed) const;
};
} // namespace modeld
