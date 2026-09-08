#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "cJSON.h"
#include "driver/gpio.h"
#include "driver/i2s.h"
#include "esp_err.h"
#include "esp_event.h"
#include "led_strip.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lwip/err.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "nvs_flash.h"

#define WIFI_SSID "TP-Link_297C"
#define WIFI_PASS "67455676"
#define SIGNALING_HOST "192.168.0.18"
#define SIGNALING_PORT 8081
#define PEER_NAME "esp32_s3_student"
#define STUDENT_NAME "Student 1"
#define STUDENT_AUDIO_DEST_PORT 5005

#define I2S_MIC_PORT I2S_NUM_0
#define MIC_SCK 5
#define MIC_WS 6
#define MIC_SD 7

#define I2S_SPK_PORT I2S_NUM_1
#define SPK_BCLK 15
#define SPK_LRC 16
#define SPK_DIN 17

#define SAMPLE_RATE 8000
#define BUFFER_SAMPLES 256
#define VOLUME 70
#define STATUS_LED_GPIO 48
#define TEACHER_AUDIO_PORT 5004
#define STUDENT_AUDIO_SSRC_BASE 0x53545544UL

static const char *TAG = "esp_webrtc_s3";
static esp_websocket_client_handle_t s_ws_client = NULL;
static EventGroupHandle_t s_wifi_event_group;
static led_strip_handle_t s_status_led;
static int s_teacher_audio_socket = -1;
static int s_student_audio_socket = -1;
static struct sockaddr_in s_student_audio_dest = {0};
static bool s_student_audio_dest_ready = false;
static bool s_remote_muted = false;
static uint16_t s_student_audio_sequence = 0;
static uint32_t s_student_audio_timestamp = 0;
static uint32_t s_student_audio_ssrc = STUDENT_AUDIO_SSRC_BASE;
static volatile uint32_t s_student_rtp_packets = 0;
static volatile uint32_t s_student_rtp_send_failures = 0;
static volatile uint32_t s_teacher_rtp_packets = 0;
static volatile uint32_t s_teacher_rtp_invalid = 0;
static volatile uint32_t s_wifi_disconnects = 0;

#define RTP_HEADER_MIN_SIZE       12
#define RTP_MAX_PAYLOAD_BYTES     512
#define AUDIO_JITTER_SLOTS        24
#define AUDIO_STARTUP_PACKETS     4
#define AUDIO_MISSING_TIMEOUT_MS  30

typedef struct {
    bool used;
    uint16_t sequence;
    uint16_t sample_count;
    int16_t pcm[RTP_MAX_PAYLOAD_BYTES];
} audio_jitter_slot_t;

static audio_jitter_slot_t s_jitter[AUDIO_JITTER_SLOTS];
static SemaphoreHandle_t s_jitter_mutex = NULL;
static uint16_t s_expected_sequence = 0;
static bool s_have_expected_sequence = false;
static bool s_playback_started = false;
static TickType_t s_last_packet_tick = 0;
static TickType_t s_missing_since_tick = 0;
static TickType_t s_last_missing_log_tick = 0;

static const char *reset_reason_name(esp_reset_reason_t reason)
{
    switch (reason) {
        case ESP_RST_POWERON: return "power-on";
        case ESP_RST_EXT: return "external";
        case ESP_RST_SW: return "software";
        case ESP_RST_PANIC: return "panic";
        case ESP_RST_INT_WDT: return "interrupt-watchdog";
        case ESP_RST_TASK_WDT: return "task-watchdog";
        case ESP_RST_WDT: return "other-watchdog";
        case ESP_RST_DEEPSLEEP: return "deep-sleep";
        case ESP_RST_BROWNOUT: return "brownout";
        case ESP_RST_SDIO: return "sdio";
        default: return "unknown";
    }
}

static void initialize_device_identity(void)
{
    uint8_t mac[6] = {0};
    if (esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK) {
        s_student_audio_ssrc = STUDENT_AUDIO_SSRC_BASE ^
                               ((uint32_t)mac[2] << 24) ^
                               ((uint32_t)mac[3] << 16) ^
                               ((uint32_t)mac[4] << 8) ^
                               (uint32_t)mac[5];
        if (s_student_audio_ssrc == 0) {
            s_student_audio_ssrc = STUDENT_AUDIO_SSRC_BASE;
        }
        ESP_LOGI(TAG, "[DEVICE] MAC=%02X:%02X:%02X:%02X:%02X:%02X SSRC=0x%08lX",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                 (unsigned long)s_student_audio_ssrc);
    }
}

static void reset_teacher_audio_stream(void)
{
    if (s_jitter_mutex != NULL &&
        xSemaphoreTake(s_jitter_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        memset(s_jitter, 0, sizeof(s_jitter));
        s_expected_sequence = 0;
        s_have_expected_sequence = false;
        s_playback_started = false;
        s_last_packet_tick = xTaskGetTickCount();
        s_missing_since_tick = 0;
        s_last_missing_log_tick = 0;
        xSemaphoreGive(s_jitter_mutex);
    } else {
        s_playback_started = false;
    }
}

static int rtp_get_payload(const uint8_t *packet, int len,
                           const uint8_t **payload, int *payload_len,
                           uint16_t *sequence)
{
    if (packet == NULL || payload == NULL || payload_len == NULL || sequence == NULL ||
        len < RTP_HEADER_MIN_SIZE) {
        return -1;
    }

    const uint8_t version = (packet[0] >> 6) & 0x03;
    if (version != 2) {
        return -1;
    }

    const bool padding = (packet[0] & 0x20) != 0;
    const bool extension = (packet[0] & 0x10) != 0;
    const uint8_t csrc_count = packet[0] & 0x0F;
    const uint8_t payload_type = packet[1] & 0x7F;

    /* This receiver is explicitly configured for RTP/PCMU (payload type 0). */
    if (payload_type != 0) {
        return -1;
    }

    *sequence = ((uint16_t)packet[2] << 8) | packet[3];

    int offset = RTP_HEADER_MIN_SIZE + ((int)csrc_count * 4);
    if (offset > len) {
        return -1;
    }

    if (extension) {
        /*
         * RTP header extension:
         *   16-bit profile, followed by 16-bit length in 32-bit words.
         */
        if (offset + 4 > len) {
            return -1;
        }

        uint16_t extension_words = ((uint16_t)packet[offset + 2] << 8) |
                                   packet[offset + 3];
        int extension_bytes = 4 + ((int)extension_words * 4);

        if (offset + extension_bytes > len) {
            return -1;
        }
        offset += extension_bytes;
    }

    int end = len;

    if (padding) {
        uint8_t padding_bytes = packet[len - 1];
        if (padding_bytes == 0 || padding_bytes > (len - offset)) {
            return -1;
        }
        end -= padding_bytes;
    }

    if (end <= offset) {
        return -1;
    }

    *payload = packet + offset;
    *payload_len = end - offset;
    return 0;
}

static int16_t mu_law_to_linear(uint8_t value)
{
    /*
     * G.711 PCMU decode.
     * Keep the standard decoded range; volume is controlled separately.
     */
    uint8_t decoded = (uint8_t)(~value);
    uint8_t sign = decoded & 0x80;
    uint8_t exponent = (decoded >> 4) & 0x07;
    uint8_t mantissa = decoded & 0x0F;

    int32_t sample = ((int32_t)(mantissa << 3) + 132) << exponent;
    if (sign) {
        sample = -sample;
    }

    if (sample > 32767) {
        sample = 32767;
    } else if (sample < -32768) {
        sample = -32768;
    }

    return (int16_t)sample;
}

__attribute__((unused)) static uint8_t linear_to_mu_law(int16_t sample);

static void setup_teacher_audio_receiver(void)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Failed to create teacher audio socket");
        return;
    }

    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    int rcvbuf = 16 * 1024;
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(TEACHER_AUDIO_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "Failed to bind teacher audio socket to port %d",
                 TEACHER_AUDIO_PORT);
        close(sock);
        return;
    }

    s_teacher_audio_socket = sock;

    if (s_jitter_mutex == NULL) {
        s_jitter_mutex = xSemaphoreCreateMutex();
        ESP_ERROR_CHECK(s_jitter_mutex == NULL ? ESP_ERR_NO_MEM : ESP_OK);
    }

    reset_teacher_audio_stream();

    ESP_LOGI(TAG, "Teacher audio receiver listening on UDP port %d",
             TEACHER_AUDIO_PORT);
}

static void setup_student_mic_sender(void)
{
    s_student_audio_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_student_audio_socket < 0) {
        ESP_LOGE(TAG, "Failed to create student microphone socket");
        return;
    }

    memset(&s_student_audio_dest, 0, sizeof(s_student_audio_dest));
    s_student_audio_dest.sin_family = AF_INET;
    s_student_audio_dest.sin_port = htons(STUDENT_AUDIO_DEST_PORT);
    s_student_audio_dest.sin_addr.s_addr = inet_addr(SIGNALING_HOST);
    s_student_audio_dest_ready = false;

    if (s_student_audio_dest.sin_addr.s_addr == INADDR_NONE) {
        ESP_LOGE(TAG, "Invalid student audio destination IP: %s", SIGNALING_HOST);
        close(s_student_audio_socket);
        s_student_audio_socket = -1;
        return;
    }

    int sndbuf = 32 * 1024;
    setsockopt(s_student_audio_socket, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

    ESP_LOGI(TAG, "Student mic RTP sender socket ready; waiting for audio transport announcement");
}

static void apply_student_audio_destination(const char *ip, uint16_t port)
{
    if (ip == NULL || s_student_audio_socket < 0) {
        return;
    }

    memset(&s_student_audio_dest, 0, sizeof(s_student_audio_dest));
    s_student_audio_dest.sin_family = AF_INET;
    s_student_audio_dest.sin_port = htons(port);
    s_student_audio_dest.sin_addr.s_addr = inet_addr(ip);

    if (s_student_audio_dest.sin_addr.s_addr == INADDR_NONE) {
        ESP_LOGW(TAG, "Ignoring invalid audio destination IP: %s", ip);
        s_student_audio_dest_ready = false;
        return;
    }

    s_student_audio_dest_ready = true;
    ESP_LOGI(TAG, "Student mic RTP target updated to %s:%d", ip, port);
}

static void student_mic_tx_task(void *arg)
{
    (void)arg;

    uint8_t payload[BUFFER_SAMPLES];
    uint8_t packet[12 + sizeof(payload)];
    int16_t raw_samples[BUFFER_SAMPLES];

    while (1) {
        if (s_student_audio_socket < 0 || !s_student_audio_dest_ready) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        size_t bytes_read = 0;
        esp_err_t err = i2s_read(I2S_MIC_PORT,
                                 raw_samples,
                                 sizeof(raw_samples),
                                 &bytes_read,
                                 pdMS_TO_TICKS(50));

        if (err != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        int sample_count = (int)bytes_read / (int)sizeof(raw_samples[0]);
        if (sample_count <= 0) {
            continue;
        }

        if (s_remote_muted) {
            memset(raw_samples, 0, sizeof(raw_samples));
        }

        int payload_len = 0;
        for (int i = 0; i < sample_count; i++) {
            payload[payload_len++] = linear_to_mu_law(raw_samples[i]);
        }

        memset(packet, 0, sizeof(packet));
        packet[0] = 0x80;
        packet[1] = 0x00;
        packet[2] = (uint8_t)((s_student_audio_sequence >> 8) & 0xFF);
        packet[3] = (uint8_t)(s_student_audio_sequence & 0xFF);
        packet[4] = (uint8_t)((s_student_audio_timestamp >> 24) & 0xFF);
        packet[5] = (uint8_t)((s_student_audio_timestamp >> 16) & 0xFF);
        packet[6] = (uint8_t)((s_student_audio_timestamp >> 8) & 0xFF);
        packet[7] = (uint8_t)(s_student_audio_timestamp & 0xFF);
        packet[8] = (uint8_t)((s_student_audio_ssrc >> 24) & 0xFF);
        packet[9] = (uint8_t)((s_student_audio_ssrc >> 16) & 0xFF);
        packet[10] = (uint8_t)((s_student_audio_ssrc >> 8) & 0xFF);
        packet[11] = (uint8_t)(s_student_audio_ssrc & 0xFF);
        memcpy(&packet[12], payload, payload_len);

        int sent = sendto(s_student_audio_socket,
                          packet,
                          12 + payload_len,
                          0,
                          (struct sockaddr *)&s_student_audio_dest,
                          sizeof(s_student_audio_dest));

        if (sent <= 0) {
            s_student_rtp_send_failures++;
            ESP_LOGW(TAG, "Failed to send student mic RTP packet: %d", errno);
        } else {
            s_student_rtp_packets++;
        }

        s_student_audio_sequence++;
        s_student_audio_timestamp += (uint32_t)payload_len;
    }
}

static int jitter_count_locked(void)
{
    int count = 0;
    for (int i = 0; i < AUDIO_JITTER_SLOTS; i++) {
        if (s_jitter[i].used) {
            count++;
        }
    }
    return count;
}

__attribute__((unused)) static void jitter_flush_locked(void)
{
    memset(s_jitter, 0, sizeof(s_jitter));
    s_have_expected_sequence = false;
    s_playback_started = false;
}

static bool sequence_is_older(uint16_t a, uint16_t b)
{
    /*
     * RTP sequence numbers wrap at 65535. This comparison is valid for
     * distances smaller than half the sequence space.
     */
    return (int16_t)(a - b) < 0;
}

static void jitter_insert(const int16_t *pcm, int sample_count, uint16_t sequence)
{
    if (pcm == NULL || sample_count <= 0 ||
        sample_count > RTP_MAX_PAYLOAD_BYTES ||
        s_jitter_mutex == NULL) {
        return;
    }

    if (xSemaphoreTake(s_jitter_mutex, pdMS_TO_TICKS(10)) != pdTRUE) {
        return;
    }

    /*
     * First packet establishes the initial sequence. Subsequent packets are
     * kept by sequence number so Wi-Fi reordering does not directly become
     * audible.
     */
    if (!s_have_expected_sequence) {
        s_expected_sequence = sequence;
        s_have_expected_sequence = true;
    }

    /* Drop packets that are already behind the playback point. */
    if (s_playback_started &&
        sequence_is_older(sequence, s_expected_sequence)) {
        xSemaphoreGive(s_jitter_mutex);
        return;
    }

    int free_slot = -1;

    for (int i = 0; i < AUDIO_JITTER_SLOTS; i++) {
        if (s_jitter[i].used && s_jitter[i].sequence == sequence) {
            memcpy(s_jitter[i].pcm, pcm, sample_count * sizeof(int16_t));
            s_jitter[i].sample_count = (uint16_t)sample_count;
            xSemaphoreGive(s_jitter_mutex);
            return;
        }

        if (!s_jitter[i].used && free_slot < 0) {
            free_slot = i;
        }
    }

    if (free_slot < 0) {
        /*
         * Buffer full. Remove the oldest queued packet, then insert the new
         * packet. This is preferable to blocking the network receiver.
         */
        int oldest = -1;
        for (int i = 0; i < AUDIO_JITTER_SLOTS; i++) {
            if (!s_jitter[i].used) {
                continue;
            }

            if (oldest < 0 ||
                sequence_is_older(s_jitter[i].sequence,
                                  s_jitter[oldest].sequence)) {
                oldest = i;
            }
        }

        if (oldest >= 0) {
            free_slot = oldest;
        }
    }

    if (free_slot >= 0) {
        s_jitter[free_slot].used = true;
        s_jitter[free_slot].sequence = sequence;
        s_jitter[free_slot].sample_count = (uint16_t)sample_count;
        memcpy(s_jitter[free_slot].pcm, pcm,
               sample_count * sizeof(int16_t));
    }

    s_last_packet_tick = xTaskGetTickCount();

    xSemaphoreGive(s_jitter_mutex);
}

static bool jitter_pop_expected(int16_t *pcm, int *sample_count)
{
    if (pcm == NULL || sample_count == NULL || s_jitter_mutex == NULL) {
        return false;
    }

    bool found = false;

    if (xSemaphoreTake(s_jitter_mutex, pdMS_TO_TICKS(10)) != pdTRUE) {
        return false;
    }

    int best = -1;

    for (int i = 0; i < AUDIO_JITTER_SLOTS; i++) {
        if (s_jitter[i].used && s_jitter[i].sequence == s_expected_sequence) {
            best = i;
            break;
        }
    }

    if (best >= 0) {
        int count = s_jitter[best].sample_count;
        memcpy(pcm, s_jitter[best].pcm, count * sizeof(int16_t));
        *sample_count = count;
        s_jitter[best].used = false;
        s_jitter[best].sample_count = 0;
        s_expected_sequence++;
        s_missing_since_tick = 0;
        found = true;
    }

    xSemaphoreGive(s_jitter_mutex);
    return found;
}

static bool jitter_ready_to_start(void)
{
    if (s_jitter_mutex == NULL) {
        return false;
    }

    bool ready = false;

    if (xSemaphoreTake(s_jitter_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        ready = s_have_expected_sequence &&
                jitter_count_locked() >= AUDIO_STARTUP_PACKETS;
        xSemaphoreGive(s_jitter_mutex);
    }

    return ready;
}

static bool jitter_should_skip_missing(void)
{
    if (s_jitter_mutex == NULL) {
        return false;
    }

    bool skip = false;

    if (xSemaphoreTake(s_jitter_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        int queued = jitter_count_locked();
        TickType_t now = xTaskGetTickCount();

        if (queued == 0) {
            s_missing_since_tick = 0;
        } else {
            if (s_missing_since_tick == 0) {
                s_missing_since_tick = now;
            } else if ((now - s_missing_since_tick) >=
                       pdMS_TO_TICKS(AUDIO_MISSING_TIMEOUT_MS)) {
                /*
                 * If a burst of RTP packets was lost, incrementing the
                 * expected sequence one packet at a time can leave playback
                 * permanently behind the packets already in the jitter
                 * buffer. Jump directly to the oldest queued packet instead.
                 */
                int next = -1;
                for (int i = 0; i < AUDIO_JITTER_SLOTS; i++) {
                    if (!s_jitter[i].used) {
                        continue;
                    }

                    if (next < 0 || sequence_is_older(s_jitter[i].sequence,
                                                       s_jitter[next].sequence)) {
                        next = i;
                    }
                }

                if (next >= 0) {
                    s_expected_sequence = s_jitter[next].sequence;
                } else {
                    s_expected_sequence++;
                }
                s_missing_since_tick = now;
                skip = true;
            }
        }

        xSemaphoreGive(s_jitter_mutex);
    }

    return skip;
}

static void teacher_audio_playback_task(void *arg)
{
    (void)arg;

    int16_t pcm[RTP_MAX_PAYLOAD_BYTES];
    int16_t silence[RTP_MAX_PAYLOAD_BYTES] = {0};

    while (1) {
        if (!s_playback_started) {
            if (!jitter_ready_to_start()) {
                /*
                 * Keep the I2S peripheral fed with silence while the initial
                 * jitter buffer fills. This avoids stale DMA data.
                 */
                size_t written = 0;
                i2s_write(I2S_SPK_PORT, silence, sizeof(silence),
                          &written, pdMS_TO_TICKS(100));
                continue;
            }

            s_playback_started = true;
            ESP_LOGI(TAG, "Speaker jitter buffer started");
        }

        int sample_count = 0;

        if (jitter_pop_expected(pcm, &sample_count)) {
            size_t bytes_written = 0;
            esp_err_t err = i2s_write(I2S_SPK_PORT,
                                      pcm,
                                      sample_count * sizeof(int16_t),
                                      &bytes_written,
                                      pdMS_TO_TICKS(100));
            if (err != ESP_OK || bytes_written !=
                (size_t)sample_count * sizeof(int16_t)) {
                ESP_LOGW(TAG, "I2S write incomplete: err=%s wrote=%u expected=%u",
                         esp_err_to_name(err),
                         (unsigned)bytes_written,
                         (unsigned)(sample_count * sizeof(int16_t)));
            }
        } else {
            if (jitter_should_skip_missing()) {
                TickType_t now = xTaskGetTickCount();
                if (s_last_missing_log_tick == 0 ||
                    (now - s_last_missing_log_tick) >= pdMS_TO_TICKS(1000)) {
                    ESP_LOGW(TAG, "RTP packet missing; inserting silence");
                    s_last_missing_log_tick = now;
                }
            }

            /*
             * One 20-ms audio period at 8 kHz is 160 samples. Writing a
             * fixed silence period prevents the I2S stream from stopping
             * while waiting for a delayed/lost UDP packet.
             */
            size_t bytes_written = 0;
            i2s_write(I2S_SPK_PORT,
                      silence,
                      160 * sizeof(int16_t),
                      &bytes_written,
                      pdMS_TO_TICKS(100));
        }
    }
}

static void teacher_audio_rx_task(void *arg)
{
    (void)arg;

    uint8_t packet[2048];
    int16_t pcm[RTP_MAX_PAYLOAD_BYTES];

    while (1) {
        if (s_teacher_audio_socket < 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        struct sockaddr_in remote_addr;
        socklen_t addr_len = sizeof(remote_addr);

        int len = recvfrom(s_teacher_audio_socket,
                           packet,
                           sizeof(packet),
                           0,
                           (struct sockaddr *)&remote_addr,
                           &addr_len);

        if (len < RTP_HEADER_MIN_SIZE) {
            s_teacher_rtp_invalid++;
            continue;
        }

        const uint8_t *payload = NULL;
        int payload_len = 0;
        uint16_t sequence = 0;

        if (rtp_get_payload(packet,
                            len,
                            &payload,
                            &payload_len,
                            &sequence) != 0) {
            s_teacher_rtp_invalid++;
            continue;
        }

        /*
         * PCMU has one 8-bit μ-law byte per 8-kHz PCM sample.
         * Limit the packet to our fixed jitter-buffer capacity.
         */
        if (payload_len <= 0 || payload_len > RTP_MAX_PAYLOAD_BYTES) {
            s_teacher_rtp_invalid++;
            ESP_LOGW(TAG, "RTP PCMU payload too large: %d bytes", payload_len);
            continue;
        }

        s_teacher_rtp_packets++;

        for (int i = 0; i < payload_len; i++) {
            int32_t sample = mu_law_to_linear(payload[i]);

            /*
             * Single, explicit volume stage.
             * VOLUME=100 means unity; values above 100 are allowed but are
             * clipped safely below.
             */
            sample = (sample * VOLUME) / 100;

            if (sample > 32767) {
                sample = 32767;
            } else if (sample < -32768) {
                sample = -32768;
            }

            pcm[i] = (int16_t)sample;
        }

        jitter_insert(pcm, payload_len, sequence);
    }
}

static void set_status_led(uint8_t red, uint8_t green)
{
    if (s_status_led == NULL) {
        return;
    }

    ESP_ERROR_CHECK(led_strip_set_pixel(s_status_led, 0, red, green, 0));
    ESP_ERROR_CHECK(led_strip_refresh(s_status_led));
}

static void setup_status_led(void)
{
    led_strip_config_t led_config = {
        .strip_gpio_num = STATUS_LED_GPIO,
        .max_leds = 1,
        .led_pixel_format = LED_PIXEL_FORMAT_GRB,
        .led_model = LED_MODEL_WS2812,
        .flags = { .invert_out = false },
    };
    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = 64,
        .flags = { .with_dma = false },
    };

    ESP_ERROR_CHECK(led_strip_new_rmt_device(&led_config, &rmt_config, &s_status_led));
    set_status_led(255, 0);
}

__attribute__((unused)) static uint8_t linear_to_mu_law(int16_t sample)
{
    const int16_t bias = 132;
    const int16_t clip = 32635;
    uint8_t sign = 0;
    if (sample < 0) {
        sign = 0x80;
        sample = -sample;
    }
    if (sample > clip) {
        sample = clip;
    }
    sample += bias;
    uint8_t exponent = 7;
    for (int16_t mask = 0x4000; (sample & mask) == 0 && exponent > 0; mask >>= 1) {
        exponent--;
    }
    const uint8_t mantissa = (sample >> (exponent + 3)) & 0x0f;
    return (uint8_t)(~(sign | (exponent << 4) | mantissa));
}

static void setup_microphone(void)
{
    i2s_config_t config = {
        .mode = I2S_MODE_MASTER | I2S_MODE_RX,
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 64,
        .use_apll = false,
        .tx_desc_auto_clear = false,
        .fixed_mclk = 0,
        .bits_per_chan = I2S_BITS_PER_CHAN_DEFAULT
    };

    i2s_pin_config_t pins = {
        .bck_io_num = MIC_SCK,
        .ws_io_num = MIC_WS,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num = MIC_SD,
    };

    ESP_ERROR_CHECK(i2s_driver_install(I2S_MIC_PORT, &config, 0, NULL));
    ESP_ERROR_CHECK(i2s_set_pin(I2S_MIC_PORT, &pins));
    ESP_ERROR_CHECK(i2s_zero_dma_buffer(I2S_MIC_PORT));
    ESP_LOGI(TAG, "INMP441 microphone configured");
}

static void setup_speaker(void)
{
    i2s_config_t config = {
        .mode = I2S_MODE_MASTER | I2S_MODE_TX,
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 64,
        .use_apll = false,
        .tx_desc_auto_clear = true,
        .fixed_mclk = 0,
        .bits_per_chan = I2S_BITS_PER_CHAN_DEFAULT
    };

    i2s_pin_config_t pins = {
        .bck_io_num = SPK_BCLK,
        .ws_io_num = SPK_LRC,
        .data_out_num = SPK_DIN,
        .data_in_num = I2S_PIN_NO_CHANGE,
    };

    ESP_ERROR_CHECK(i2s_driver_install(I2S_SPK_PORT, &config, 0, NULL));
    ESP_ERROR_CHECK(i2s_set_pin(I2S_SPK_PORT, &pins));
    ESP_ERROR_CHECK(i2s_zero_dma_buffer(I2S_SPK_PORT));
    ESP_LOGI(TAG, "MAX98357A speaker configured");
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data)
{
    if (event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "Wi-Fi station started");
        esp_wifi_connect();
    } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *event = (wifi_event_sta_disconnected_t *)event_data;
        xEventGroupClearBits(s_wifi_event_group, BIT0);
        set_status_led(255, 0);
        s_wifi_disconnects++;
        ESP_LOGW(TAG, "[WIFI] Disconnected reason=%d; reconnecting",
                 event ? event->reason : -1);
        esp_wifi_connect();
    } else if (event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "[WIFI] Connected IP=" IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, BIT0);
        set_status_led(0, 255);
    }
}

static void wifi_init(void)
{
    s_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(s_wifi_event_group == NULL ? ESP_ERR_NO_MEM : ESP_OK);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    ESP_ERROR_CHECK(sta_netif == NULL ? ESP_ERR_NO_MEM : ESP_OK);

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    wifi_config_t wifi_cfg = {0};
    strncpy((char *)wifi_cfg.sta.ssid, WIFI_SSID, sizeof(wifi_cfg.sta.ssid) - 1);
    strncpy((char *)wifi_cfg.sta.password, WIFI_PASS, sizeof(wifi_cfg.sta.password) - 1);
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_cfg.sta.pmf_cfg.capable = true;
    wifi_cfg.sta.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
}

static const char *build_offer_sdp(void)
{
    static char offer[512];
    snprintf(offer, sizeof(offer),
             "v=0\r\n"
             "o=- 0 0 IN IP4 127.0.0.1\r\n"
             "s=-\r\n"
             "t=0 0\r\n"
             "a=group:BUNDLE 0\r\n"
             "a=msid-semantic: WMS *\r\n"
             "m=audio 9 UDP/TLS/RTP/SAVPF 0\r\n"
             "c=IN IP4 0.0.0.0\r\n"
             "a=rtcp:9 IN IP4 0.0.0.0\r\n"
             "a=mid:0\r\n"
             "a=sendrecv\r\n"
             "a=rtcp-mux\r\n"
             "a=setup:actpass\r\n"
             "a=ice-ufrag:esp32s3\r\n"
             "a=ice-pwd:ESP32S3WEBRTCICEPASSWORD1234567890\r\n"
             "a=fingerprint:sha-256 12:34:56:78:9A:BC:DE:F0:12:34:56:78:9A:BC:DE:F0:12:34:56:78:9A:BC:DE:F0:12:34:56:78:9A:BC:DE:F0\r\n"
             "a=rtpmap:0 PCMU/8000\r\n");
    return offer;
}

static void send_offer_to_bridge(void)
{
    if (s_ws_client == NULL || !esp_websocket_client_is_connected(s_ws_client)) {
        return;
    }

    uint8_t mac[6] = {0};
    char peer_id[48] = {0};
    if (esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK) {
        snprintf(peer_id, sizeof(peer_id), "%s_%02X%02X%02X%02X%02X%02X",
                 PEER_NAME, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    } else {
        snprintf(peer_id, sizeof(peer_id), "%s", PEER_NAME);
    }

    cJSON *root = cJSON_CreateObject();
    esp_netif_ip_info_t ip_info;
    char device_ip[16] = {0};
    cJSON_AddStringToObject(root, "type", "offer");
    cJSON_AddStringToObject(root, "peerId", peer_id);
    cJSON_AddStringToObject(root, "studentName", STUDENT_NAME);
    cJSON_AddNumberToObject(root, "audioSsrc", (double)s_student_audio_ssrc);
    cJSON_AddNumberToObject(root, "audioReceivePort", TEACHER_AUDIO_PORT);
    if (esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"), &ip_info) == ESP_OK) {
        snprintf(device_ip, sizeof(device_ip), IPSTR, IP2STR(&ip_info.ip));
        cJSON_AddStringToObject(root, "ip", device_ip);
    }
    cJSON_AddStringToObject(root, "sdp", build_offer_sdp());

    char *payload = cJSON_PrintUnformatted(root);
    if (payload != NULL) {
        ESP_LOGI(TAG, "Sending SDP offer to WebRTC bridge");
        esp_websocket_client_send_text(s_ws_client, payload, strlen(payload), portMAX_DELAY);
        cJSON_free(payload);
    }

    cJSON_Delete(root);
}

static void process_answer_json(const char *payload)
{
    cJSON *root = cJSON_Parse(payload);
    if (root == NULL) {
        ESP_LOGE(TAG, "Invalid answer JSON from bridge: %s", payload);
        return;
    }

    cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (type != NULL && strcmp(type->valuestring, "answer") == 0) {
        cJSON *sdp = cJSON_GetObjectItemCaseSensitive(root, "sdp");
        if (sdp != NULL && sdp->valuestring != NULL) {
            ESP_LOGI(TAG, "Bridge answered with SDP: %s", sdp->valuestring);
        }
    } else if (type != NULL && strcmp(type->valuestring, "remote_mute") == 0) {
        cJSON *enabled = cJSON_GetObjectItemCaseSensitive(root, "enabled");
        if (enabled != NULL && cJSON_IsBool(enabled)) {
            s_remote_muted = cJSON_IsTrue(enabled);
            ESP_LOGI(TAG, "Student microphone %s", s_remote_muted ? "muted" : "unmuted");
        }
    } else if (type != NULL && strcmp(type->valuestring, "audio_transport") == 0) {
        cJSON *ip = cJSON_GetObjectItemCaseSensitive(root, "ip");
        cJSON *port = cJSON_GetObjectItemCaseSensitive(root, "port");
        if (ip != NULL && ip->valuestring != NULL && port != NULL && cJSON_IsNumber(port)) {
            apply_student_audio_destination(ip->valuestring, (uint16_t)port->valueint);
        }
    } else if (type != NULL && strcmp(type->valuestring, "error") == 0) {
        cJSON *message = cJSON_GetObjectItemCaseSensitive(root, "message");
        ESP_LOGE(TAG, "Bridge error: %s", message ? message->valuestring : "unknown");
    }

    cJSON_Delete(root);
}

static void websocket_event_handler(void *handler_args, esp_event_base_t base,
                                   int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "[WS] Connected to %s:%d", SIGNALING_HOST, SIGNALING_PORT);
            reset_teacher_audio_stream();
            s_student_audio_dest_ready = false;
            send_offer_to_bridge();
            break;

        case WEBSOCKET_EVENT_DATA: {
            if (data->data_ptr == NULL || data->data_len <= 0) {
                break;
            }

            char *payload = malloc(data->data_len + 1);
            if (payload == NULL) {
                ESP_LOGE(TAG, "Out of memory while reading WebSocket payload");
                break;
            }

            memcpy(payload, data->data_ptr, data->data_len);
            payload[data->data_len] = '\0';
            process_answer_json(payload);
            free(payload);
            break;
        }

        case WEBSOCKET_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "[WS] Disconnected; retrying later");
            reset_teacher_audio_stream();
            s_student_audio_dest_ready = false;
            break;

        case WEBSOCKET_EVENT_ERROR:
            if (data != NULL) {
                ESP_LOGE(TAG, "[WS] Error type=%d tls=%d stack=%d errno=%d http=%d",
                         data->error_handle.error_type,
                         data->error_handle.esp_tls_last_esp_err,
                         data->error_handle.esp_tls_stack_err,
                         data->error_handle.esp_transport_sock_errno,
                         data->error_handle.esp_ws_handshake_status_code);
            } else {
                ESP_LOGE(TAG, "[WS] Error without details");
            }
            break;

        default:
            break;
    }
}

static void health_monitor_task(void *arg)
{
    (void)arg;

    while (1) {
        wifi_ap_record_t ap = {0};
        esp_err_t wifi_status = esp_wifi_sta_get_ap_info(&ap);
        bool ws_connected = s_ws_client != NULL &&
                            esp_websocket_client_is_connected(s_ws_client);
        ESP_LOGI(TAG,
                 "[HEALTH] heap=%lu min_heap=%lu wifi=%s rssi=%d ws=%s tx=%lu tx_fail=%lu rx=%lu rx_invalid=%lu wifi_disc=%lu",
                 (unsigned long)esp_get_free_heap_size(),
                 (unsigned long)esp_get_minimum_free_heap_size(),
                 wifi_status == ESP_OK ? "connected" : "disconnected",
                 wifi_status == ESP_OK ? ap.rssi : 0,
                 ws_connected ? "connected" : "disconnected",
                 (unsigned long)s_student_rtp_packets,
                 (unsigned long)s_student_rtp_send_failures,
                 (unsigned long)s_teacher_rtp_packets,
                 (unsigned long)s_teacher_rtp_invalid,
                 (unsigned long)s_wifi_disconnects);
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

static void signaling_client_task(void *arg)
{
    ESP_LOGI(TAG, "Starting WebSocket signaling client");

    while (1) {
        xEventGroupWaitBits(s_wifi_event_group, BIT0, pdFALSE, pdTRUE, portMAX_DELAY);

        static char signaling_uri[96];
        snprintf(signaling_uri, sizeof(signaling_uri), "ws://%s:%d", SIGNALING_HOST, SIGNALING_PORT);

        esp_websocket_client_config_t ws_cfg = {
            .uri = signaling_uri,
            .buffer_size = 4096,
            .transport = WEBSOCKET_TRANSPORT_OVER_TCP,
            .network_timeout_ms = 15000,
            .reconnect_timeout_ms = 3000,
            .pingpong_timeout_sec = 30,
            .ping_interval_sec = 20,
            .disable_auto_reconnect = true,
        };

        s_ws_client = esp_websocket_client_init(&ws_cfg);
        esp_websocket_register_events(s_ws_client, WEBSOCKET_EVENT_ANY, websocket_event_handler, NULL);
        esp_websocket_client_set_ping_interval_sec(s_ws_client, 20);
        esp_websocket_client_start(s_ws_client);

        while (s_ws_client != NULL && esp_websocket_client_is_connected(s_ws_client)) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }

        ESP_LOGW(TAG, "WebSocket session ended; waiting before reconnect");
        if (s_ws_client != NULL) {
            esp_websocket_client_destroy(s_ws_client);
            s_ws_client = NULL;
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

void app_main(void)
{
    esp_reset_reason_t reset_reason = esp_reset_reason();
    ESP_LOGI(TAG, "[BOOT] Reset reason=%d (%s) heap=%lu min_heap=%lu",
             (int)reset_reason,
             reset_reason_name(reset_reason),
             (unsigned long)esp_get_free_heap_size(),
             (unsigned long)esp_get_minimum_free_heap_size());

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    setup_status_led();
    wifi_init();
    initialize_device_identity();
    setup_microphone();
    setup_speaker();

    setup_teacher_audio_receiver();
    setup_student_mic_sender();
    xTaskCreate(signaling_client_task, "sig_task", 8192, NULL, 5, NULL);
    xTaskCreate(teacher_audio_rx_task, "teacher_rx_task", 8192, NULL, 6, NULL);
    xTaskCreate(teacher_audio_playback_task, "teacher_play_task", 8192, NULL, 5, NULL);
    xTaskCreate(student_mic_tx_task, "student_mic_tx_task", 8192, NULL, 5, NULL);
    xTaskCreate(health_monitor_task, "health_monitor_task", 4096, NULL, 2, NULL);

    ESP_LOGI(TAG, "ESP-IDF bidirectional audio path initialized (teacher receive + student mic transmit)");
}
