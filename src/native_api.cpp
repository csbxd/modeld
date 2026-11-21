#include "modeld/core.hpp"
#include "modeld/media.hpp"
#include "modeld/native.h"
#include <algorithm>
#include <limits>
#include <map>
#include <mutex>
namespace {
thread_local std::string last_error;
thread_local int last_status = 500;
void record_error() {
    try {
        throw;
    } catch (const modeld::Error &e) {
        last_error = e.what();
        last_status = e.status;
    } catch (const std::exception &e) {
        last_error = e.what();
        last_status = 500;
    } catch (...) {
        last_error = "Unknown native failure";
        last_status = 500;
    }
}
struct Context {
    std::filesystem::path model, cache;
    std::mutex mutex;
    std::map<std::pair<int, int>, std::shared_ptr<modeld::Rife>> profiles;
    std::shared_ptr<modeld::Rife> get(int width, int height) {
        std::lock_guard lock(mutex);
        const auto key = std::pair(width, height);
        if (!profiles.contains(key)) {
            if (profiles.size() >= 4) {
                const auto unused =
                    std::find_if(profiles.begin(), profiles.end(),
                                 [](const auto &entry) { return entry.second.use_count() == 1; });
                if (unused == profiles.end())
                    throw modeld::Error(429, "All inference profiles are active");
                profiles.erase(unused);
            }
            profiles[key] = std::make_shared<modeld::Rife>(model, cache, width, height);
        }
        return profiles.at(key);
    }
};
struct NativeStream {
    std::shared_ptr<std::atomic<bool>> canceled;
    std::unique_ptr<modeld::MediaStream> media;
    std::string description;
    std::string content_type;
};
struct CancelToken {
    std::shared_ptr<std::atomic<bool>> value = std::make_shared<std::atomic<bool>>(false);
};
void transfer(std::unique_ptr<modeld::Segment> segment, ModeldSegment *out) {
    *out = {segment->bytes.data(), segment->bytes.size(), segment->frames, segment->seconds,
            segment.get()};
    segment.release();
}
} // namespace
extern "C" {
void *modeld_context_create(const char *model, const char *cache) {
    try {
        auto context = std::make_unique<Context>();
        context->model = model;
        context->cache = cache;
        context->get(1920, 1080);
        return context.release();
    } catch (...) {
        record_error();
        return nullptr;
    }
}
void modeld_context_free(void *context) {
    delete static_cast<Context *>(context);
}
void *modeld_cancel_token_create(void) {
    try {
        return new CancelToken;
    } catch (...) {
        record_error();
        return nullptr;
    }
}
void modeld_cancel_token_set(void *token) {
    static_cast<CancelToken *>(token)->value->store(true);
}
int modeld_cancel_token_is_set(void *token) {
    return static_cast<CancelToken *>(token)->value->load();
}
void modeld_cancel_token_free(void *token) {
    delete static_cast<CancelToken *>(token);
}
void *modeld_stream_open(void *context, const char *request_json, const char *settings_json,
                         void *cancel_token) {
    try {
        auto *backend = static_cast<Context *>(context);
        const auto request = modeld::Json::parse(request_json);
        const auto config = modeld::Json::parse(settings_json);
        modeld::require_keys(request, {"model", "video", "audio", "tracks"});
        modeld::require_keys(config, {"segment_seconds", "encoder", "qp", "source_timeout_seconds",
                                      "fragmented_mp4", "av1_vbr"});
        modeld::RenderSettings settings{config.at("segment_seconds"), config.at("qp"),
                                        config.at("source_timeout_seconds"), config.at("encoder"),
                                        config.at("fragmented_mp4"), std::nullopt};
        if (auto vbr = config.find("av1_vbr"); vbr != config.end() && !vbr->is_null()) {
            modeld::require_keys(*vbr, {"target_bps", "max_bps"});
            settings.av1_vbr = modeld::Av1Vbr{vbr->at("target_bps"), vbr->at("max_bps")};
        }
        auto video = modeld::Input::parse(request.at("video"));
        std::optional<modeld::Input> audio;
        if (request.contains("audio") && !request.at("audio").is_null())
            audio = modeld::Input::parse(request.at("audio"));
        const auto &selection = request.at("tracks");
        modeld::TrackSelection tracks{selection.at("audio"), selection.at("subtitles")};
        auto stream = std::make_unique<NativeStream>();
        stream->canceled = static_cast<CancelToken *>(cancel_token)->value;
        stream->media = std::make_unique<modeld::MediaStream>(
            std::move(video), std::move(audio), tracks, settings,
            [backend](int width, int height) { return backend->get(width, height); },
            stream->canceled);
        stream->description = stream->media->description().dump(
            -1, ' ', false, modeld::Json::error_handler_t::replace);
        stream->content_type = stream->media->content_type();
        return stream.release();
    } catch (...) {
        record_error();
        return nullptr;
    }
}
const char *modeld_stream_description(void *stream) {
    return static_cast<NativeStream *>(stream)->description.c_str();
}
const char *modeld_stream_content_type(void *stream) {
    return static_cast<NativeStream *>(stream)->content_type.c_str();
}
int modeld_stream_init(void *stream, ModeldSegment *out) {
    try {
        auto segment = std::make_unique<modeld::Segment>();
        segment->bytes = static_cast<NativeStream *>(stream)->media->initialization();
        transfer(std::move(segment), out);
        return 0;
    } catch (...) {
        record_error();
        return -1;
    }
}
int modeld_stream_render(void *stream, uint64_t index, ModeldSegment *out) {
    try {
        if (index > std::numeric_limits<int64_t>::max() / 600)
            throw modeld::Error(404, "Segment does not exist");
        transfer(
            std::make_unique<modeld::Segment>(
                static_cast<NativeStream *>(stream)->media->render(static_cast<int64_t>(index))),
            out);
        return 0;
    } catch (...) {
        record_error();
        return -1;
    }
}
void modeld_stream_cancel(void *stream) {
    static_cast<NativeStream *>(stream)->canceled->store(true);
}
void modeld_stream_free(void *stream) {
    delete static_cast<NativeStream *>(stream);
}
void modeld_segment_free(ModeldSegment *out) {
    delete static_cast<modeld::Segment *>(out->owner);
    *out = {};
}
const char *modeld_last_error(void) {
    return last_error.c_str();
}
int modeld_last_status(void) {
    return last_status;
}
}
