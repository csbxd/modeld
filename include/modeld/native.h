#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    const uint8_t *data;
    size_t length;
    int frames;
    double seconds;
    void *owner;
} ModeldSegment;
void *modeld_context_create(const char *model, const char *cache);
void modeld_context_free(void *context);
void *modeld_cancel_token_create(void);
void modeld_cancel_token_set(void *token);
int modeld_cancel_token_is_set(void *token);
void modeld_cancel_token_free(void *token);
void *modeld_stream_open(void *context, const char *request_json, const char *settings_json,
                         void *cancel_token);
const char *modeld_stream_description(void *stream);
const char *modeld_stream_content_type(void *stream);
int modeld_stream_init(void *stream, ModeldSegment *result);
int modeld_stream_render(void *stream, uint64_t index, ModeldSegment *result);
void modeld_stream_cancel(void *stream);
void modeld_stream_free(void *stream);
void modeld_segment_free(ModeldSegment *result);
const char *modeld_last_error(void);
int modeld_last_status(void);
#ifdef __cplusplus
}
#endif
