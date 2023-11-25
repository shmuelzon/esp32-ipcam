#ifndef AUDIO_ENCODER_H
#define AUDIO_ENCODER_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    AUDIO_CODEC_OPUS,
} audio_codec_t;

typedef void (*audio_encoder_frame_free_func_t)(void *ctx);

int audio_encoder_encode(int16_t *samples, size_t number_of_samples,
    int64_t timestamp, audio_encoder_frame_free_func_t free_func, void *ctx);
int audio_encoder_get_encoded(uint8_t **data, size_t *length,
    int64_t *timestamp);

size_t audio_encoder_frame_size(audio_codec_t codec, uint32_t sample_rate);
int audio_encoder_initialize(audio_codec_t codec, uint32_t sample_rate);

#endif
