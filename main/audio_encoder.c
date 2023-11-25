#include "audio_encoder.h"
#include "rtp.h"
#include <esp_log.h>
#include <opus.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <stddef.h>

static const char *TAG = "Audio Encoder";
static const size_t opus_frame_length_ms = 20;
static const size_t opus_max_combined_frames = 120 /* ms */ / opus_frame_length_ms;
static const size_t opus_max_packet_size = 3 * 1276;

typedef struct {
    int16_t *samples;
    size_t number_of_samples;
    int64_t timestamp;
    audio_encoder_frame_free_func_t free_func;
    void *ctx;
} frame_t;

typedef struct {
    uint8_t *data;
    size_t length;
    int64_t timestamp;
} packet_t;

typedef struct audio_encoder_t audio_encoder_t;

typedef struct {
    audio_codec_t codec;
    int (*init)(audio_encoder_t *audio_encoder);
    size_t (*required_task_stack_size)(audio_encoder_t *audio_encoder);
    size_t (*required_frame_size)(uint32_t sample_rate);
    int (*encode)(audio_encoder_t *audio_encoder, frame_t *frame);
} audio_encoder_ops_t;

typedef struct audio_encoder_t {
    uint32_t sample_rate;
    audio_encoder_ops_t *ops;
    union {
        struct {
            OpusEncoder *encoder;
            OpusRepacketizer *repacketizer;
            size_t current_frame;
            uint8_t **pending_frames;
            int64_t first_frame_timestamp;
        } opus;
    };
} audio_encoder_t;

static QueueHandle_t frames_queue;
static QueueHandle_t packets_queue;

static int push_audio_packet(uint8_t *data, size_t length, int64_t timestamp)
{
    /* XXX TODO Should go through ipcam.c */
    return rtp_send_opus(data, length, timestamp, free, data);
}

/* Opus */
static int opus_init(audio_encoder_t *audio_encoder)
{
    int i, err = 0;

    audio_encoder->opus.encoder =
        opus_encoder_create(audio_encoder->sample_rate, 1,
        OPUS_APPLICATION_RESTRICTED_LOWDELAY, &err);
    if (err < 0)
    {
        ESP_LOGE(TAG, "Failed creating Opus encoder: %s", opus_strerror(err));
        return -1;
    }

    audio_encoder->opus.repacketizer = opus_repacketizer_create();
    if (!audio_encoder->opus.repacketizer)
    {
        ESP_LOGE(TAG, "Failed creating Opus repacketizer");
        return -1;
    }

    audio_encoder->opus.pending_frames = malloc(opus_max_combined_frames *
        sizeof(*audio_encoder->opus.pending_frames));
    for (i = 0; i < opus_max_combined_frames; i++)
        audio_encoder->opus.pending_frames[i] = malloc(opus_max_packet_size);

    return 0;
}

static int opus_combine_packets(audio_encoder_t *audio_encoder)
{
    size_t max_combined_length = 1277 * opus_repacketizer_get_nb_frames(
        audio_encoder->opus.repacketizer);
    uint8_t *data = malloc(max_combined_length);
    int32_t ret = opus_repacketizer_out(
        audio_encoder->opus.repacketizer, data, max_combined_length);

    opus_repacketizer_init(audio_encoder->opus.repacketizer);
    if (ret < 0)
    {
        ESP_LOGE(TAG, "Failed creating combined packet: %s", opus_strerror(ret));
        free(data);
        return -1;
    }

    return push_audio_packet(data, ret,
        audio_encoder->opus.first_frame_timestamp);
}

static int opus_encode_frame(audio_encoder_t *audio_encoder, frame_t *frame)
{
    int ret;

    if (audio_encoder->opus.current_frame == 0)
        audio_encoder->opus.first_frame_timestamp = frame->timestamp;

    ret = opus_encode(audio_encoder->opus.encoder, frame->samples,
        frame->number_of_samples,
        audio_encoder->opus.pending_frames[audio_encoder->opus.current_frame],
        opus_max_packet_size);
    if (ret < 0)
    {
        ESP_LOGE(TAG, "Failed to encode Opus: %s", opus_strerror(ret));
        return -1;
    }

    if ((ret = opus_repacketizer_cat(audio_encoder->opus.repacketizer,
        audio_encoder->opus.pending_frames[audio_encoder->opus.current_frame],
        ret)))
    {
        ESP_LOGE(TAG, "Failed concatenating Opus packet: %s", opus_strerror(ret));
        return -1;
    }

    audio_encoder->opus.current_frame++;
    if (audio_encoder->opus.current_frame != opus_max_combined_frames)
        return 0;

    audio_encoder->opus.current_frame = 0;
    return opus_combine_packets(audio_encoder);
}

static size_t opus_required_task_stack_size(audio_encoder_t *audio_encoder)
{
    return 24576;
}

static size_t opus_required_frame_size(uint32_t sample_rate)
{
    return sample_rate * (opus_frame_length_ms / 1000.0);
}

static audio_encoder_ops_t opus_encoder = {
    .codec = AUDIO_CODEC_OPUS,
    .init = opus_init,
    .required_task_stack_size = opus_required_task_stack_size,
    .required_frame_size = opus_required_frame_size,
    .encode = opus_encode_frame,
};

/* Common */
static audio_encoder_ops_t *audio_encoder_ops[] = {
    &opus_encoder,
    NULL
};

static audio_encoder_ops_t *get_audio_encoder_ops(audio_codec_t codec)
{
    audio_encoder_ops_t **ops;

    for (ops = audio_encoder_ops; *ops; ops++)
    {
        if ((*ops)->codec == codec)
            return *ops;
    }

    return NULL;
}

static void audio_encoder_task(void *pvParameter)
{
    audio_encoder_t *encoder = (audio_encoder_t *)pvParameter;
    frame_t frame;

    while (1)
    {
        if (xQueueReceive(frames_queue, &frame, portMAX_DELAY) != pdTRUE)
            continue;

        encoder->ops->encode(encoder, &frame);
        frame.free_func(frame.ctx);
    }
    vTaskDelete(NULL);
}

int audio_encoder_encode(int16_t *samples, size_t number_of_samples,
    int64_t timestamp, audio_encoder_frame_free_func_t free_func, void *ctx)
{
    frame_t frame = {
        .samples = samples,
        .number_of_samples = number_of_samples,
        .timestamp = timestamp,
        .free_func = free_func,
        .ctx = ctx,
    };

    return xQueueSend(frames_queue, &frame, 0) != pdTRUE;
}

int audio_encoder_get_encoded(uint8_t **data, size_t *length,
    int64_t *timestamp)
{
    packet_t packet;

    if (xQueueReceive(packets_queue, &packet, portMAX_DELAY) != pdTRUE)
        return -1;

    *data = packet.data;
    *length = packet.length;
    *timestamp = packet.timestamp;

    return 0;
}

size_t audio_encoder_frame_size(audio_codec_t codec, uint32_t sample_rate)
{
    audio_encoder_ops_t *ops = get_audio_encoder_ops(codec);

    if (ops)
        return ops->required_frame_size(sample_rate);
    return 0;
}

int audio_encoder_initialize(audio_codec_t codec, uint32_t sample_rate)
{
    audio_encoder_t *audio_encoder = calloc(1, sizeof(*audio_encoder));
    size_t task_stack_size;

    if (!audio_encoder)
    {
        ESP_LOGE(TAG, "Failed allocating encoder");
        return -1;
    }

    if (!(frames_queue = xQueueCreate(5, sizeof(frame_t))))
    {
        ESP_LOGE(TAG, "Failed creating queue");
        return -1;
    }

    if (!(packets_queue = xQueueCreate(5, sizeof(packet_t))))
    {
        ESP_LOGE(TAG, "Failed creating queue");
        return -1;
    }

    audio_encoder->sample_rate = sample_rate;
    audio_encoder->ops = get_audio_encoder_ops(codec);

    if (!audio_encoder->ops)
    {
        ESP_LOGE(TAG, "Unknown audio codec %d", codec);
        return -1;
    }

    if (audio_encoder->ops->init(audio_encoder))
    {
        ESP_LOGE(TAG, "Failed initializing audio encoder");
        return -1;
    }

    task_stack_size =
        audio_encoder->ops->required_task_stack_size(audio_encoder);

    if (xTaskCreatePinnedToCore(audio_encoder_task, "audio_encoder_task",
        task_stack_size, audio_encoder, 5, NULL, 1) != pdPASS)
    {
        ESP_LOGE(TAG, "Failed creating audio encoder task");
        return -1;
    }

    return 0;
}
