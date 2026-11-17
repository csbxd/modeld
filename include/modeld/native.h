#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    int width, height;
    double duration, source_fps;
} ModeldVideoInfo;
typedef struct {
    const uint8_t *data;
    size_t length;
    int frames;
    double seconds;
    void *owner;
} ModeldSegment;
void *modeld_context_create(const char *model, const char *cache);
void modeld_context_free(void *context);
void *modeld_video_open(void *context, const char *url, int segment_seconds, int qp,
                        const char *encoder, ModeldVideoInfo *info);
void modeld_video_cancel(void *video);
void modeld_video_free(void *video);
int modeld_video_render(void *video, int64_t sequence, ModeldSegment *result);
void modeld_segment_free(ModeldSegment *result);
const char *modeld_last_error(void);
int modeld_last_status(void);
#ifdef __cplusplus
}
#endif
