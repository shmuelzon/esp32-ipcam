#include "rtp.h"
#include "wifi.h"
#include <esp_camera.h>
#include <esp_err.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <freertos/timers.h>
#include <freertos/queue.h>
#include <sys/socket.h>
#include <string.h>
#include <endian.h>

#define RTP_PT_JPEG 26 /* From RFC1890 */
#define RTP_JPEG_RESTART 0x40 /* From RFC2435 */
#define PACKET_SIZE 1300

#if BYTE_ORDER != BIG_ENDIAN && BYTE_ORDER != LITTLE_ENDIAN
#error "Couldn't detect endianess"
#endif

#if BYTE_ORDER == BIG_ENDIAN
#define htobe24(x) (x)
#define be24toh(x) (x)
#else
#define htobe24(x) (((x) & 0xff) << 16 | ((x) & 0xff00) | (((x) & 0xff0000) >> 16))
#define be24toh(x) (((x) & 0xff0000) >> 16 | ((x) & 0xff00) | ((x) & 0xff) << 16)
#endif

/* RTP Header */
typedef struct {
#if BYTE_ORDER == BIG_ENDIAN
    uint8_t version:2;   /* Protocol version */
    uint8_t p:1;         /* Padding flag */
    uint8_t x:1;         /* Header extension flag */
    uint8_t cc:4;        /* CSRC count */
    uint8_t m:1;         /* Marker bit */
    uint8_t pt:7;        /* Payload type */
#else
    uint8_t cc:4;
    uint8_t x:1;
    uint8_t p:1;
    uint8_t version:2;
    uint8_t pt:7;
    uint8_t m:1;
#endif
    uint16_t seq;        /* Sequence number */
    uint32_t ts;         /* Timestamp */
    uint32_t ssrc;       /* Synchronization source */
    uint32_t csrc[0];    /* Optional CSRC list */
} __attribute__((packed)) rtp_hdr_t;

typedef struct {
#if BYTE_ORDER == BIG_ENDIAN
    uint32_t off:24;     /* Fragment byte offset */
    uint32_t tspec:8;    /* Type-specific field */
#else
    uint32_t tspec:8;
    uint32_t off:24;
#endif
    uint8_t type;        /* ID of JPEG decoder params */
    uint8_t q;           /* Quantization factor (or table ID) */
    uint8_t width;       /* Frame width in 8 pixel blocks */
    uint8_t height;      /* Frame height in 8 pixel blocks */
} __attribute__((packed)) jpeg_hdr_t;

typedef struct {
    uint8_t mbz;
    uint8_t precision;
    uint16_t length;
} __attribute__((packed)) jpeg_hdr_qtable_t;

typedef enum {
    FRAME_TYPE_JPEG,
    FRAME_TYPE_OPUS,
} frame_type_t;

typedef struct {
    frame_type_t type;
    int64_t timestamp;
    const uint8_t *buffer;
    size_t length;
    union {
        struct {
            int width;
            int height;
        } jpeg;
    };
    rtp_frame_free_func_t free_func;
    void *free_ctx;    
} frame_t;

static const char *TAG = "RTP";
static const size_t video_queue_size = 10;
static const size_t audio_queue_size = 10;

static int video_socket = -1, audio_socket = -1;
static uint8_t ttl = 1;
static QueueHandle_t video_queue, audio_queue;
static SemaphoreHandle_t queue_semaphore;

static int create_socket(const char *destination, uint16_t port)
{
    struct sockaddr_in dst = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
    };
    int sock = -1;

    if (!port)
        goto Error;

    if (!inet_pton(AF_INET, destination, &dst.sin_addr))
    {
        ESP_LOGE(TAG, "Failed parsing IP address");
        goto Error;
    }

    if((sock = socket(AF_INET, SOCK_DGRAM, 0)) < 0)
    {
        ESP_LOGE(TAG, "Failed creating socket: %d (%m)", errno);
        goto Error;
    }

    if (IN_MULTICAST(ntohl(dst.sin_addr.s_addr)))
    {
        if (setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl,
            sizeof(uint8_t)))
        {
            ESP_LOGE(TAG, "Failed setting multicast TTL");
            goto Error;
        }
    }

    if (connect(sock, (struct sockaddr *)&dst, sizeof(dst)) < 0)
    {
        ESP_LOGE(TAG, "Failed connecting to destination: %d (%m)", errno);
        goto Error;
    }

    return sock;

Error:
    if (sock >= 0)
        close(sock);

    return -1;
}

static int parse_jpeg(frame_t *frame, uint8_t const **lqt, uint8_t const **cqt,
    uint8_t const **scan, size_t *len)
{
    const uint8_t *ptr = frame->buffer;

    while (ptr - frame->buffer < frame->length)
    {
        /* Make sure we're at the start of a sequence */
        if (ptr[0] != 0xff)
            return -1;

        switch (ptr[1])
        {
        case 0xd8: /* Start Of Image */
            ptr += 2; /* Skip segment code */
            break;
        case 0xdb: /* Define Quantization Table(s) */
            if (ptr[4] == 0)
                *lqt = ptr + 5;
            else if (ptr[4] == 1)
                *cqt = ptr + 5;
            /* segment code (2) + length(2) + destination (1) + 64 = 69 */
            ptr += 69;
            break;
        case 0xda: /* Start Of Scan */
            *scan = ptr + 2 + (ptr[2] << 8 | ptr[3]);
            *len = frame->length - (*scan - frame->buffer) - 2 /* EOI (2) */;
            return 0;
        case 0xc0: /* Start Of Frame (baseline DCT) */
        case 0xc4: /* Define Huffman Table(s) */
        case 0xe0: /* Application-specific 0 */
            ptr += 2 + (ptr[2] << 8 | ptr[3]);
            break;
        default:
            ESP_LOGE(TAG, "Got unhandled marker 0x%02x", ptr[1]);
            return -1;
        }
    }

    return 0;
}

/* Adapted from https://tools.ietf.org/html/rfc2435, appendix C */
static int rtp_send_jpeg_data(uint16_t start_seq, uint32_t ts, uint32_t ssrc,
    const uint8_t *jpeg_data, size_t len, uint8_t type, uint8_t typespec,
    int width, int height, uint8_t q, const uint8_t *lqt, const uint8_t *cqt)
{
    uint8_t packet_buf[PACKET_SIZE];
    rtp_hdr_t *rtp_hdr = (rtp_hdr_t *)packet_buf;
    jpeg_hdr_t *jpg_hdr = (jpeg_hdr_t *)((uint8_t *)rtp_hdr + sizeof(rtp_hdr_t));
    jpeg_hdr_qtable_t *qtbl_hdr = (jpeg_hdr_qtable_t *)((uint8_t *)jpg_hdr + sizeof(jpeg_hdr_t));
    uint8_t *ptr;
    size_t bytes_left = len;
    size_t data_len;

    /* Initialize RTP header */
    rtp_hdr->version = 2;
    rtp_hdr->p = 0;
    rtp_hdr->x = 0;
    rtp_hdr->cc = 0;
    rtp_hdr->m = 0;
    rtp_hdr->pt = RTP_PT_JPEG;
    rtp_hdr->seq = htobe16(start_seq);
    rtp_hdr->ts = htobe32(ts);
    rtp_hdr->ssrc = htobe32(ssrc);

    /* Initialize JPEG header */
    jpg_hdr->tspec = typespec;
    jpg_hdr->off = 0;
    jpg_hdr->type = type;
    jpg_hdr->q = q;
    jpg_hdr->width = width / 8;
    jpg_hdr->height = height / 8;

    /* Initialize quantization table header */
    if (q >= 128)
    {
        qtbl_hdr->mbz = 0;
        qtbl_hdr->precision = 0; /* This code uses 8 bit tables only */
        qtbl_hdr->length = htobe16(128); /* 2 64-byte tables */
    };

    while (bytes_left > 0)
    {
        if (q >= 128 && jpg_hdr->off == 0)
        {
            ptr = (uint8_t *)qtbl_hdr + sizeof(jpeg_hdr_qtable_t);
            memcpy(ptr, lqt, 64);
            ptr += 64;
            memcpy(ptr, cqt, 64);
            ptr += 64;
        }
        else
            ptr = (uint8_t *)jpg_hdr + sizeof(jpeg_hdr_t);

        data_len = PACKET_SIZE - (ptr - packet_buf);
        if (data_len >= bytes_left)
        {
            data_len = bytes_left;
            rtp_hdr->m = 1;
        }

        memcpy(ptr, jpeg_data + be24toh(jpg_hdr->off), data_len);

        if (send(video_socket, packet_buf, (ptr - packet_buf) + data_len, 0)
            < 0)
        {
            ESP_LOGE(TAG, "Failed sending JPEG packet: %d (%s)", errno, strerror(errno));
            goto Error;
        }

        jpg_hdr->off = htobe24(be24toh(jpg_hdr->off) + data_len);
        bytes_left -= data_len;
        rtp_hdr->seq = htobe16(be16toh(rtp_hdr->seq) + 1);
    }

Error:
    return be16toh(rtp_hdr->seq);
}

static int rtp_send_jpeg_frame(frame_t *frame)
{
    static uint16_t sequence_number = 0;
    uint32_t timestamp, ssrc = 0xdeadbeef;
    const uint8_t *lqt = NULL, *cqt = NULL, *jpeg_data = NULL;
    size_t len = 0;
    uint8_t q = 128;

    if (parse_jpeg(frame, &lqt, &cqt, &jpeg_data, &len))
    {
        ESP_LOGE(TAG, "Failed parsing JPEG data");
        return -1;
    }

    /* Verify quantization table were found */
    if (!lqt || !cqt)
        q = 0;

    timestamp = frame->timestamp * 90000 /* Hz */ / 1000000;
    sequence_number = rtp_send_jpeg_data(sequence_number, timestamp, ssrc,
        jpeg_data, len, 0, 0, frame->jpeg.width, frame->jpeg.height, q, lqt, cqt);

    return 0;
}

static int rtp_send_opus_frame(frame_t *frame)
{
    static uint16_t sequence_number = 0;
    uint32_t ssrc = 0xdeadbabe;

    uint8_t packet_buf[PACKET_SIZE];
    rtp_hdr_t *rtp_hdr = (rtp_hdr_t *)packet_buf;

    /* Initialize RTP header */
    rtp_hdr->version = 2;
    rtp_hdr->p = 0;
    rtp_hdr->x = 0;
    rtp_hdr->cc = 0;
    rtp_hdr->m = 1;
    rtp_hdr->pt = 97;
    rtp_hdr->seq = htobe16(sequence_number++);
    rtp_hdr->ts = htobe32(frame->timestamp * 48000 /* Hz */ / 1000000);
    rtp_hdr->ssrc = htobe32(ssrc);

    if (frame->length > 1000)
    {
        ESP_LOGE(TAG, "Packet to long!!! %d", frame->length);
        return 0;
    }

    memcpy(packet_buf + sizeof(*rtp_hdr), frame->buffer, frame->length);
    if (send(audio_socket, packet_buf, sizeof(*rtp_hdr) + frame->length, 0) < 0)
    {
        ESP_LOGE(TAG, "Failed sending Opus packet: %d (%s)", errno, strerror(errno));
    }

    return 0;
}

static void stream_task(void *pvParameter)
{
    frame_t frame;

    while (1)
    {
        if (xSemaphoreTake(queue_semaphore, portMAX_DELAY) != pdTRUE)
            continue;

        if (xQueueReceive(audio_queue, &frame, 0) != pdTRUE)
        {
            if (xQueueReceive(video_queue, &frame, 0) != pdTRUE)
                continue;
        }

        switch (frame.type)
        {
        case FRAME_TYPE_JPEG: rtp_send_jpeg_frame(&frame); break;
        case FRAME_TYPE_OPUS: rtp_send_opus_frame(&frame); break;
        }

        /* Free frame */
        if (frame.free_func)
            frame.free_func(frame.free_ctx);
    };

    vTaskDelete(NULL);
}

static int add_frame_to_queue(frame_t *frame)
{
    static QueueHandle_t queue;

    switch (frame->type)
    {
    case FRAME_TYPE_JPEG:
        queue = video_queue;
        break;
    case FRAME_TYPE_OPUS:
        queue = audio_queue;
        break;
    };

    if (xQueueSend(queue, frame, 0) != pdTRUE)
    {
        ESP_LOGE(TAG, "Video queue full!");
        return -1;
    }

    if (xSemaphoreGive(queue_semaphore) != pdTRUE)
    {
        ESP_LOGE(TAG, "Faile giving semaphore");
        return -1;
    }

    return 0;
}

int rtp_send_jpeg(int width, int height, const uint8_t *buffer, size_t length,
    int64_t timestamp, rtp_frame_free_func_t free_func, void *ctx)
{
    frame_t jpeg_frame = {
        .type = FRAME_TYPE_JPEG,
        .timestamp = timestamp,
        .buffer = buffer,
        .length = length,
        .jpeg = {
            .width = width,
            .height = height,
        },
        .free_func = free_func,
        .free_ctx = ctx,
    };

    return add_frame_to_queue(&jpeg_frame);
}

int rtp_send_opus(const uint8_t *buffer, size_t length, int64_t timestamp,
    rtp_frame_free_func_t free_func, void *ctx)
{
    frame_t opus_frame = {
        .type = FRAME_TYPE_OPUS,
        .timestamp = timestamp,
        .buffer = buffer,
        .length = length,
        .free_func = free_func,
        .free_ctx = ctx,
    };

    return add_frame_to_queue(&opus_frame);
}

void rtp_ttl_set(uint8_t _ttl)
{
    ttl = _ttl;
}

int rtp_initialize(const char *destination, uint16_t video_port,
    uint16_t audio_port)
{
    ESP_LOGD(TAG, "Initializing RTP");

    video_socket = create_socket(destination, video_port);
    audio_socket = create_socket(destination, audio_port);

    if (video_socket < 0 || (audio_port && audio_socket < 0))
    {
        ESP_LOGE(TAG, "Failed creating sockets");
        return -1;
    }

    video_queue = xQueueCreate(video_queue_size, sizeof(frame_t));
    audio_queue = xQueueCreate(audio_queue_size, sizeof(frame_t));

    if (!video_queue || !audio_queue)
    {
        ESP_LOGE(TAG, "Failed creating queues");
        return -1;
    }

    if (!(queue_semaphore = xSemaphoreCreateCounting(
        video_queue_size + audio_queue_size, 0)))
    {
        ESP_LOGE(TAG, "Failed creating semaphore");
        return -1;
    }
    
    if (xTaskCreatePinnedToCore(stream_task, "stream_task", 4096, NULL, 5,
        NULL, 1) != pdPASS)
    {
        ESP_LOGE(TAG, "Failed creating stream task");
        return -1;
    }

    return 0;
}
