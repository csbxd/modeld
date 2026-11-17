#include "modeld/common.hpp"
#include "modeld/media.hpp"
#include "modeld/native.h"
#include <algorithm>
#include <map>
#include <mutex>
namespace {
thread_local std::string last_error;
thread_local int last_status = 500;
void error() {
    try {
        throw;
    } catch (const modeld::Error &e) {
        last_error = e.what();
        last_status = e.status;
    } catch (const std::exception &e) {
        last_error = e.what();
        last_status = 500;
    } catch (...) {
        last_error = "Unknown native inference failure";
        last_status = 500;
    }
}
struct Context {
    std::filesystem::path model, cache;
    std::mutex mutex;
    std::map<std::pair<int, int>, std::shared_ptr<modeld::Rife>> models;
    std::shared_ptr<modeld::Rife> get(int w, int h) {
        std::lock_guard lock(mutex);
        auto key = std::pair(w, h);
        if (!models.contains(key)) {
            if (models.size() >= 4) {
                auto unused = std::find_if(models.begin(), models.end(), [](const auto &entry) {
                    return entry.second.use_count() == 1;
                });
                if (unused == models.end())
                    throw modeld::Error(429, "All resident model profiles are active");
                models.erase(unused);
            }
            models[key] = std::make_shared<modeld::Rife>(model, cache, w, h);
        }
        return models.at(key);
    }
};
struct Video {
    std::shared_ptr<std::atomic<bool>> cancelled = std::make_shared<std::atomic<bool>>(false);
    std::unique_ptr<modeld::Video> media;
};
} // namespace
extern "C" {
void *modeld_context_create(const char *model, const char *cache) {
    try {
        auto c = std::make_unique<Context>();
        c->model = model;
        c->cache = cache;
        c->get(1920, 1080);
        return c.release();
    } catch (...) {
        error();
        return nullptr;
    }
}
void modeld_context_free(void *c) {
    delete static_cast<Context *>(c);
}
void *modeld_video_open(void *context, const char *url, const char *source_headers, int seconds,
                        int qp, const char *encoder, ModeldVideoInfo *info) {
    try {
        auto *c = static_cast<Context *>(context);
        auto v = std::make_unique<Video>();
        v->media = std::make_unique<modeld::Video>(
            url, source_headers ? source_headers : "", seconds, qp, encoder,
            [c](int w, int h) { return c->get(w, h); }, v->cancelled);
        auto i = v->media->info();
        *info = {i.width, i.height, i.duration, i.source_fps};
        return v.release();
    } catch (...) {
        error();
        return nullptr;
    }
}
void modeld_video_cancel(void *video) {
    static_cast<Video *>(video)->cancelled->store(true);
}
void modeld_video_free(void *video) {
    delete static_cast<Video *>(video);
}
int modeld_video_render(void *video, int64_t sequence, ModeldSegment *out) {
    try {
        auto s =
            std::make_unique<modeld::Segment>(static_cast<Video *>(video)->media->render(sequence));
        out->data = s->bytes.data();
        out->length = s->bytes.size();
        out->frames = s->frames;
        out->seconds = s->seconds;
        out->owner = s.release();
        return 0;
    } catch (...) {
        error();
        return -1;
    }
}
void modeld_segment_free(ModeldSegment *out) {
    delete static_cast<modeld::Segment *>(out->owner);
    *out = {};
}
const char *modeld_last_error() {
    return last_error.c_str();
}
int modeld_last_status() {
    return last_status;
}
}
