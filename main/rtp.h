#ifndef RTP_H
#define RTP_H

#include <stdint.h>
#include <stddef.h>

typedef void (*rtp_frame_free_func_t)(void *ctx);

int rtp_send_jpeg(int width, int height, const uint8_t *buffer, size_t length,
    int64_t timestamp, rtp_frame_free_func_t free_func, void *ctx);
int rtp_send_opus(const uint8_t *buffer, size_t length, int64_t timestamp,
    rtp_frame_free_func_t free_func, void *ctx);

void rtp_ttl_set(uint8_t ttl);

int rtp_initialize(const char *destination, uint16_t video_port,
    uint16_t audio_port);

#endif
