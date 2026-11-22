#include <Arduino.h>
#include <WiFi.h>
#include <lwip/sockets.h>
#include <errno.h>
#include <driver/i2s_std.h>
#include <freertos/ringbuf.h>
#include <math.h>
#include "protocol.h"

// ---- FILL THESE IN ----
const char* WIFI_SSID = "PH01";
const char* WIFI_PASS = "boomboom";
const char* SERVER_IP = "10.155.162.197";

static const uint16_t SERVER_PORT      = 8000;
static const uint16_t LOCAL_UDP_PORT   = 8001;
static const uint16_t PROTOCOL_MAGIC   = 0xAE05;
static const uint8_t  MSG_TYPE_MIC     = 1;
static const uint8_t  MSG_TYPE_TTS     = 2;

#define MIC_SCK   33
#define MIC_WS    32
#define MIC_SD    35

#define AMP_BCLK  26
#define AMP_LRC   25
#define AMP_DOUT  27

static const uint32_t MIC_SAMPLE_RATE   = 16000;
static const uint32_t AMP_SAMPLE_RATE   = 24000;
static const size_t   MIC_SAMPLES       = 512;
static const size_t   RINGBUF_BYTES     = 65536; // ~1.36 s jitter buffer @ 24 kHz
static const size_t   PREBUF_WATERMARK  = 12288; // ~256 ms pre-buffer before playback
static const float    PLAYBACK_GAIN     = 0.45f; // Crisp desk volume, USB-safe

static i2s_chan_handle_t rx_handle      = NULL;
static i2s_chan_handle_t tx_handle      = NULL;
static RingbufHandle_t   audioRingBuf   = NULL;

static int                udpSock       = -1;
static struct sockaddr_in serverAddr    = {};

static volatile bool     isSpeaking     = false;
static volatile uint32_t lastSpeakEndMs = 0;

bool initMicI2S() {
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num  = 6;
    chan_cfg.dma_frame_num = 256;

    if (i2s_new_channel(&chan_cfg, NULL, &rx_handle) != ESP_OK) return false;

    i2s_std_config_t std_cfg = {};
    std_cfg.clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(MIC_SAMPLE_RATE);
    std_cfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO);
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;

    std_cfg.gpio_cfg.mclk = I2S_GPIO_UNUSED;
    std_cfg.gpio_cfg.bclk = (gpio_num_t)MIC_SCK;
    std_cfg.gpio_cfg.ws   = (gpio_num_t)MIC_WS;
    std_cfg.gpio_cfg.dout = I2S_GPIO_UNUSED;
    std_cfg.gpio_cfg.din  = (gpio_num_t)MIC_SD;
    std_cfg.gpio_cfg.invert_flags.mclk_inv = false;
    std_cfg.gpio_cfg.invert_flags.bclk_inv = false;
    std_cfg.gpio_cfg.invert_flags.ws_inv   = false;

    if (i2s_channel_init_std_mode(rx_handle, &std_cfg) != ESP_OK) return false;
    return i2s_channel_enable(rx_handle) == ESP_OK;
}

bool initAmpI2S() {
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num  = 8;
    chan_cfg.dma_frame_num = 256;

    if (i2s_new_channel(&chan_cfg, &tx_handle, NULL) != ESP_OK) return false;

    // 16-bit STEREO (duplicating mono sample into L+R) avoids ESP32 16-bit mono FIFO swap crackle
    i2s_std_config_t std_cfg = {};
    std_cfg.clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(AMP_SAMPLE_RATE);
    std_cfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;

    std_cfg.gpio_cfg.mclk = I2S_GPIO_UNUSED;
    std_cfg.gpio_cfg.bclk = (gpio_num_t)AMP_BCLK;
    std_cfg.gpio_cfg.ws   = (gpio_num_t)AMP_LRC;
    std_cfg.gpio_cfg.dout = (gpio_num_t)AMP_DOUT;
    std_cfg.gpio_cfg.din  = I2S_GPIO_UNUSED;
    std_cfg.gpio_cfg.invert_flags.mclk_inv = false;
    std_cfg.gpio_cfg.invert_flags.bclk_inv = false;
    std_cfg.gpio_cfg.invert_flags.ws_inv   = false;

    if (i2s_channel_init_std_mode(tx_handle, &std_cfg) != ESP_OK) return false;
    return i2s_channel_enable(tx_handle) == ESP_OK;
}

void flushSilence() {
    static int16_t zeros[512] = {0}; // 256 stereo frames
    for (int i = 0; i < 8; i++) {
        size_t written = 0;
        i2s_channel_write(tx_handle, zeros, sizeof(zeros), &written, pdMS_TO_TICKS(50));
    }
}

void playReadyChirp() {
    static int16_t buf[512]; // 256 stereo frames
    for (int c = 0; c < 14; c++) {
        for (size_t i = 0; i < 256; i++) {
            float t = (float)(c * 256 + i) / (float)AMP_SAMPLE_RATE;
            int16_t s = (int16_t)(2000.0f * sinf(2.0f * PI * 660.0f * t));
            buf[i * 2]     = s;
            buf[i * 2 + 1] = s;
        }
        size_t written = 0;
        i2s_channel_write(tx_handle, buf, sizeof(buf), &written, portMAX_DELAY);
    }
    flushSilence();
}

bool initUDPSocket() {
    udpSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (udpSock < 0) return false;

    struct sockaddr_in localAddr = {};
    localAddr.sin_family      = AF_INET;
    localAddr.sin_port        = htons(LOCAL_UDP_PORT);
    localAddr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(udpSock, (struct sockaddr *)&localAddr, sizeof(localAddr)) < 0) {
        close(udpSock);
        udpSock = -1;
        return false;
    }

    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port   = htons(SERVER_PORT);
    inet_pton(AF_INET, SERVER_IP, &serverAddr.sin_addr);
    return true;
}

void micTask(void *param) {
    static int32_t     stereoRaw[MIC_SAMPLES * 2];
    static AudioPacket txPkt;
    uint32_t seq = 1;

    Serial.printf("[MicTask] Streaming 16 kHz PCM -> %s:%u\n", SERVER_IP, SERVER_PORT);

    while (true) {
        size_t bytesRead = 0;
        esp_err_t err = i2s_channel_read(rx_handle, stereoRaw, sizeof(stereoRaw), &bytesRead, portMAX_DELAY);
        if (err != ESP_OK || bytesRead < sizeof(stereoRaw)) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        if (isSpeaking || (millis() - lastSpeakEndMs < 500)) {
            continue;
        }

        for (size_t i = 0; i < MIC_SAMPLES; i++) {
            txPkt.samples[i] = (int16_t)(stereoRaw[i * 2] >> 16);
        }

        txPkt.header.seq    = seq++;
        txPkt.header.magic  = PROTOCOL_MAGIC;
        txPkt.header.length = MIC_SAMPLES * sizeof(int16_t);
        txPkt.header.type   = MSG_TYPE_MIC;

        sendto(udpSock, &txPkt, sizeof(AudioPacket), 0,
               (struct sockaddr *)&serverAddr, sizeof(serverAddr));
    }
}

void netRxTask(void *param) {
    static AudioPacket rxPkt;
    uint32_t rxCount = 0;
    uint32_t drops   = 0;

    Serial.printf("[NetRx] Listening for TTS on UDP port %u\n", LOCAL_UDP_PORT);

    while (true) {
        int n = recvfrom(udpSock, &rxPkt, sizeof(rxPkt), 0, NULL, NULL);
        if (n < (int)sizeof(PacketHeader)) continue;
        if (rxPkt.header.magic != PROTOCOL_MAGIC) continue;
        if (rxPkt.header.type != MSG_TYPE_TTS) continue;
        if (rxPkt.header.length == 0 || rxPkt.header.length > 1024) continue;
        if (n < (int)(sizeof(PacketHeader) + rxPkt.header.length)) continue;

        if (xRingbufferSend(audioRingBuf, rxPkt.samples, rxPkt.header.length, 0) != pdTRUE) {
            drops++;
        } else {
            rxCount++;
            if (rxCount % 25 == 0) {
                Serial.printf("[NetRx] TTS seq=%lu rx=%lu drops=%lu\n",
                              (unsigned long)rxPkt.header.seq,
                              (unsigned long)rxCount,
                              (unsigned long)drops);
            }
        }
    }
}

void ampTask(void *param) {
    static int16_t stereoOut[512 * 2]; // 512 mono samples -> 512 stereo frames (2048 B)
    bool playing = false;
    uint32_t firstByteTime = 0;

    playReadyChirp();
    Serial.println("[AmpTask] Speaker ready (24 kHz stereo-dup).");

    while (true) {
        if (!playing) {
            UBaseType_t waiting = 0;
            vRingbufferGetInfo(audioRingBuf, NULL, NULL, NULL, NULL, &waiting);

            if (waiting == 0) {
                firstByteTime = 0;
                vTaskDelay(pdMS_TO_TICKS(5));
                continue;
            }
            if (firstByteTime == 0) firstByteTime = millis();

            if (waiting >= PREBUF_WATERMARK || (millis() - firstByteTime) > 350) {
                playing = true;
                isSpeaking = true;
                Serial.printf("[AmpTask] Playing reply (buffered=%u B)\n", (unsigned)waiting);
            } else {
                vTaskDelay(pdMS_TO_TICKS(5));
                continue;
            }
        }

        size_t itemSize = 0;
        void *item = xRingbufferReceiveUpTo(audioRingBuf, &itemSize, pdMS_TO_TICKS(500), 1024);
        if (item != NULL && itemSize >= 2) {
            size_t samples = itemSize / sizeof(int16_t);
            const int16_t *src = (const int16_t *)item;
            for (size_t i = 0; i < samples; i++) {
                int16_t s = (int16_t)((float)src[i] * PLAYBACK_GAIN);
                stereoOut[i * 2]     = s;
                stereoOut[i * 2 + 1] = s;
            }
            vRingbufferReturnItem(audioRingBuf, item);

            size_t written = 0;
            i2s_channel_write(tx_handle, stereoOut, samples * 2 * sizeof(int16_t), &written, portMAX_DELAY);
        } else {
            if (item != NULL) vRingbufferReturnItem(audioRingBuf, item);
            flushSilence();
            playing = false;
            isSpeaking = false;
            lastSpeakEndMs = millis();
            firstByteTime = 0;
            Serial.println("[AmpTask] Done speaking. Mic active again.");
        }
    }
}

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.printf("\n=== DeskAI Voice Node (Free heap: %lu B) ===\n", (unsigned long)ESP.getFreeHeap());

    audioRingBuf = xRingbufferCreate(RINGBUF_BYTES, RINGBUF_TYPE_BYTEBUF);
    if (!audioRingBuf || !initMicI2S() || !initAmpI2S()) {
        Serial.println("ERROR: RingBuf or I2S init failed!");
        while (true) delay(1000);
    }

    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.print("Connecting to Wi-Fi");
    while (WiFi.status() != WL_CONNECTED) {
        delay(300);
        Serial.print(".");
    }
    Serial.printf("\nConnected! ESP32 IP: %s\n", WiFi.localIP().toString().c_str());

    if (!initUDPSocket()) {
        Serial.println("ERROR: UDP socket init failed!");
        while (true) delay(1000);
    }

    xTaskCreatePinnedToCore(micTask,   "MicTask",   8192, NULL, 2, NULL, 0);
    xTaskCreatePinnedToCore(netRxTask, "NetRxTask", 8192, NULL, 3, NULL, 0);
    xTaskCreatePinnedToCore(ampTask,   "AmpTask",   8192, NULL, 3, NULL, 1);
}

void loop() {
    vTaskDelay(pdMS_TO_TICKS(1000));
}