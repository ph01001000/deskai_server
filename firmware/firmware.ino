#include <Arduino.h>
#include <WiFi.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <lwip/sockets.h>
#include <errno.h>
#include <driver/i2s_std.h>
#include <freertos/ringbuf.h>
#include <math.h>
#include "protocol.h"
#include "secrets.h"

// =========================================================
// SLEEK INTERFACE + SHARP ROBOEYES TUNING
// =========================================================
static const int      BASE_EYE_W        = 28;    // Sharp square-ish width
static const int      BASE_EYE_H        = 20;    // Sharp square-ish height
static const int      EYE_RADIUS        = 2;     // Crisp 2px corner radius
static const int      EYE_SPACING       = 14;    // Gap between eye inner edges
static const int      EYE_CENTER_Y      = 30;    // Perfectly centered under top HUD
static const float    EYE_SMOOTHNESS    = 0.32f; // Snappy kinetic tweening
static const int      LOOK_RANGE_X      = 16;    // Wide cardinal glance X
static const int      LOOK_RANGE_Y      = 6;     // Wide cardinal glance Y
static const int      CURIOSITY_GROWTH  = 8;     // Outer eye stretches +8px on side looks!
static const uint32_t BLINK_MIN_MS      = 2600;  // Blink interval min
static const uint32_t BLINK_MAX_MS      = 4800;  // Blink interval max
static const uint32_t SLEEP_AFTER_MS    = 45000; // Screen dimming after 45s idle
static const uint32_t CAPTION_MS        = 1300;  // Caption speed
static const float    PLAYBACK_GAIN     = 1.0f; // Volume
// =========================================================

static const uint16_t SERVER_PORT           = 8000;
static const uint16_t LOCAL_UDP_PORT        = 8001;
static const uint16_t PROTOCOL_MAGIC        = 0xAE05;
static const uint8_t  MSG_TYPE_MIC          = 1;
static const uint8_t  MSG_TYPE_TTS          = 2;
static const uint8_t  MSG_TYPE_STOP         = 3;
static const uint8_t  MSG_TYPE_STATUS       = 4;
static const uint8_t  MSG_TYPE_AUDIO_CONFIG = 5;

static const uint8_t  STATE_LISTENING  = 0;
static const uint8_t  STATE_THINKING   = 1;
static const uint8_t  STATE_SPEAKING   = 2;
static const uint8_t  STATE_IDLE       = 3;

static const uint8_t  MOOD_NORMAL      = 0;
static const uint8_t  MOOD_ANNOYED     = 1;
static const uint8_t  MOOD_HAPPY       = 2;
static const uint8_t  MOOD_LAUGH       = 3;
static const uint8_t  MOOD_ANGRY       = 4;
static const uint8_t  MOOD_SAD         = 5;
static const uint8_t  MOOD_COOL        = 6;

#define MIC_SCK   33
#define MIC_WS    32
#define MIC_SD    35

#define AMP_BCLK  26
#define AMP_LRC   25
#define AMP_DOUT  27

#define OLED_SDA  21
#define OLED_SCL  22

#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1
#define I2C_ADDRESS   0x3C
#define BOOT_BTN  0

static const uint32_t MIC_SAMPLE_RATE   = 16000;
static const uint32_t AMP_SAMPLE_RATE   = 24000;
static const size_t   MIC_SAMPLES       = 512;
static const size_t   RINGBUF_BYTES     = 32768;
static const size_t   PREBUF_WATERMARK  = 10240;

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

static i2s_chan_handle_t rx_handle      = NULL;
static i2s_chan_handle_t tx_handle      = NULL;
static RingbufHandle_t   audioRingBuf   = NULL;

static int                udpSock       = -1;
static struct sockaddr_in serverAddr    = {};

static volatile bool     isSpeaking     = false;
static volatile uint32_t lastSpeakEndMs = 0;
static volatile uint8_t  micLevel       = 0;

static portMUX_TYPE      uiMux          = portMUX_INITIALIZER_UNLOCKED;
static volatile uint8_t  uiState        = STATE_IDLE;
static volatile uint8_t  uiMood         = MOOD_NORMAL;
static volatile bool     pendingIdle    = false;
static char              uiText[512]    = "";
static volatile bool     uiTextChanged  = false;
static uint32_t          currentAmpRate = AMP_SAMPLE_RATE;

struct RoboEyeRig {
    float gazeX;
    float gazeY;
    float hL, hR;
    float topL, topR;
    float tiltTop;
    float botCurve;
};

static inline float lerpf(float a, float b, float t) {
    return a + (b - a) * t;
}

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

    if (i2s_channel_init_std_mode(rx_handle, &std_cfg) != ESP_OK) return false;
    return i2s_channel_enable(rx_handle) == ESP_OK;
}

bool initAmpI2S() {
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num  = 8;
    chan_cfg.dma_frame_num = 256;
    chan_cfg.auto_clear    = true;

    if (i2s_new_channel(&chan_cfg, &tx_handle, NULL) != ESP_OK) return false;

    i2s_std_config_t std_cfg = {};
    std_cfg.clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(AMP_SAMPLE_RATE);
    std_cfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;

    std_cfg.gpio_cfg.mclk = I2S_GPIO_UNUSED;
    std_cfg.gpio_cfg.bclk = (gpio_num_t)AMP_BCLK;
    std_cfg.gpio_cfg.ws   = (gpio_num_t)AMP_LRC;
    std_cfg.gpio_cfg.dout = (gpio_num_t)AMP_DOUT;
    std_cfg.gpio_cfg.din  = I2S_GPIO_UNUSED;

    if (i2s_channel_init_std_mode(tx_handle, &std_cfg) != ESP_OK) return false;
    return i2s_channel_enable(tx_handle) == ESP_OK;
}

bool setAmpSampleRate(uint32_t newRate) {
    if (newRate == 0 || newRate == currentAmpRate) return true;
    Serial.printf("[I2S] Switching Amp Clock: %u Hz -> %u Hz\n", currentAmpRate, newRate);

    i2s_channel_disable(tx_handle);
    i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(newRate);
    esp_err_t err = i2s_channel_reconfig_std_clock(tx_handle, &clk_cfg);
    i2s_channel_enable(tx_handle);

    if (err == ESP_OK) {
        currentAmpRate = newRate;
        return true;
    }
    return false;
}

void flushSilence() {
    static int16_t zeros[512] = {0};
    for (int i = 0; i < 4; i++) {
        size_t written = 0;
        i2s_channel_write(tx_handle, zeros, sizeof(zeros), &written, pdMS_TO_TICKS(20));
    }
}

void playReadyChirp() {
    static int16_t buf[512];
    for (int c = 0; c < 6; c++) {
        for (size_t i = 0; i < 256; i++) {
            float t = (float)(c * 256 + i) / (float)AMP_SAMPLE_RATE;
            int16_t s = (int16_t)(1600.0f * sinf(2.0f * PI * 880.0f * t));
            buf[i * 2]     = s;
            buf[i * 2 + 1] = s;
        }
        size_t written = 0;
        i2s_channel_write(tx_handle, buf, sizeof(buf), &written, portMAX_DELAY);
    }
    flushSilence();
}

bool initUDPSocket() {
    if (udpSock >= 0) {
        close(udpSock);
        udpSock = -1;
    }

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

    while (true) {
        size_t bytesRead = 0;
        esp_err_t err = i2s_channel_read(rx_handle, stereoRaw, sizeof(stereoRaw), &bytesRead, portMAX_DELAY);
        if (err != ESP_OK || bytesRead < sizeof(stereoRaw)) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        if (isSpeaking || (millis() - lastSpeakEndMs < 500)) {
            micLevel = 0;
            continue;
        }

        int32_t peak = 0;
        for (size_t i = 0; i < MIC_SAMPLES; i++) {
            int16_t s = (int16_t)(stereoRaw[i * 2] >> 16);
            txPkt.samples[i] = s;
            int32_t a = (s < 0) ? -s : s;
            if (a > peak) peak = a;
        }
        int lvl = (peak * 100) / 7000;
        if (lvl > 100) lvl = 100;
        micLevel = (uint8_t)lvl;

        txPkt.header.seq    = seq++;
        txPkt.header.magic  = PROTOCOL_MAGIC;
        txPkt.header.length = MIC_SAMPLES * sizeof(int16_t);
        txPkt.header.type   = MSG_TYPE_MIC;

        if (udpSock >= 0) {
            sendto(udpSock, &txPkt, sizeof(AudioPacket), 0,
                   (struct sockaddr *)&serverAddr, sizeof(serverAddr));
        }
    }
}

void netRxTask(void *param) {
    static AudioPacket rxPkt;
    uint32_t expectedSeq = 0;

    while (true) {
        if (udpSock < 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        int n = recvfrom(udpSock, &rxPkt, sizeof(rxPkt), 0, NULL, NULL);
        if (n < (int)sizeof(PacketHeader)) continue;
        if (rxPkt.header.magic != PROTOCOL_MAGIC) continue;
        if (rxPkt.header.length == 0 || rxPkt.header.length > 1024) continue;
        if (n < (int)(sizeof(PacketHeader) + rxPkt.header.length)) continue;

        if (rxPkt.header.type == MSG_TYPE_AUDIO_CONFIG && rxPkt.header.length >= 4) {
            uint32_t newRate = *(uint32_t *)rxPkt.samples;
            setAmpSampleRate(newRate);
            expectedSeq = 0;
            continue;
        }

        if (rxPkt.header.type == MSG_TYPE_TTS) {
            uint32_t seq = rxPkt.header.seq;
            if (seq == 1 || expectedSeq == 0) expectedSeq = 1;

            if (seq >= expectedSeq) {
                expectedSeq = seq + 1;
                xRingbufferSend(audioRingBuf, rxPkt.samples, rxPkt.header.length, 0);
            }
        } else if (rxPkt.header.type == MSG_TYPE_STATUS) {
            const uint8_t *raw = (const uint8_t *)rxPkt.samples;
            uint8_t newState = raw[0];
            uint8_t newMood  = (rxPkt.header.length >= 2) ? raw[1] : MOOD_NORMAL;
            size_t textLen   = (rxPkt.header.length > 2) ? (rxPkt.header.length - 2) : 0;
            if (textLen >= sizeof(uiText)) textLen = sizeof(uiText) - 1;

            portENTER_CRITICAL(&uiMux);
            if (newState == STATE_IDLE && isSpeaking) {
                pendingIdle = true;
            } else {
                uiState = newState;
                if (newState != STATE_IDLE) uiMood = newMood;
                pendingIdle = false;
                if (newState == STATE_SPEAKING && textLen > 0) {
                    memcpy(uiText, raw + 2, textLen);
                    uiText[textLen] = '\0';
                    uiTextChanged = true;
                }
            }
            portEXIT_CRITICAL(&uiMux);
        }
    }
}

void ampTask(void *param) {
    static int16_t stereoOut[512 * 2];
    bool playing = false;
    uint32_t firstByteTime = 0;

    vTaskDelay(pdMS_TO_TICKS(500));
    playReadyChirp();

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

            portENTER_CRITICAL(&uiMux);
            if (pendingIdle || uiState == STATE_SPEAKING) {
                uiState = STATE_IDLE;
                pendingIdle = false;
            }
            portEXIT_CRITICAL(&uiMux);
        }
    }
}

// ---------------------------------------------------------
// HUD & NETWORK INTERFACE OVERLAY
// ---------------------------------------------------------

void drawTopHUD(uint8_t state) {
    // 1. Tactical corner brackets
    display.drawFastHLine(1, 1, 6, SSD1306_WHITE);
    display.drawFastVLine(1, 1, 6, SSD1306_WHITE);
    display.drawFastHLine(121, 1, 6, SSD1306_WHITE);
    display.drawFastVLine(126, 1, 6, SSD1306_WHITE);
    display.drawFastHLine(1, 62, 6, SSD1306_WHITE);
    display.drawFastVLine(1, 57, 6, SSD1306_WHITE);
    display.drawFastHLine(121, 62, 6, SSD1306_WHITE);
    display.drawFastVLine(126, 57, 6, SSD1306_WHITE);

    // 2. Network Indicator (Wi-Fi Signal Bars)
    if (WiFi.status() == WL_CONNECTED) {
        int rssi = WiFi.RSSI();
        int bars = (rssi > -65) ? 4 : (rssi > -75) ? 3 : (rssi > -85) ? 2 : 1;
        for (int b = 0; b < 4; b++) {
            if (b < bars) {
                display.drawFastVLine(10 + b * 2, 7 - b, b + 1, SSD1306_WHITE);
            }
        }
    } else {
        // Disconnected warning cross
        display.drawLine(10, 3, 16, 7, SSD1306_WHITE);
        display.drawLine(10, 7, 16, 3, SSD1306_WHITE);
    }

    // 3. Top Status Telemetry Tag
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(34, 1);

    switch (state) {
        case STATE_LISTENING: display.print("[REC//AUD]"); break;
        case STATE_THINKING:  display.print("[PROC//AI]"); break;
        case STATE_SPEAKING:  display.print("[OUT//VOX]"); break;
        default:              display.print("[SYS//OK] "); break;
    }

    // Micro grid divider dots on top right
    for (int x = 96; x < 118; x += 4) {
        display.drawPixel(x, 4, SSD1306_WHITE);
    }
}

// ---------------------------------------------------------
// SHARP SQUARE ROBOEYES (28x20, radius 2, NO CROSSHAIR)
// ---------------------------------------------------------

void drawSquareEye(int cx, int cy, int w, int h, float topCut, float tiltTop, float botCurve, bool isLeft, bool blinking) {
    if (blinking) {
        display.fillRoundRect(cx - w / 2, cy - 1, w, 2, 1, SSD1306_WHITE);
        return;
    }

    if (h < 3) h = 3;
    int x = cx - w / 2;
    int y = cy - h / 2;

    // Solid sharp rectangle with crisp 2px corner radius
    display.fillRoundRect(x, y, w, h, EYE_RADIUS, SSD1306_WHITE);

    // Eyelid top cutoff (for tired/annoyed)
    int tc = (int)roundf(topCut);
    if (tc > 0) {
        display.fillRect(x - 1, y - 1, w + 2, tc + 1, SSD1306_BLACK);
    }

    // Tilt brow cuts (for angry / cool)
    int tt = (int)roundf(tiltTop);
    if (tt > 0) {
        if (isLeft) display.fillTriangle(x - 1, y - 1, x + w + 1, y - 1, x + w + 1, y + tt, SSD1306_BLACK);
        else display.fillTriangle(x - 1, y - 1, x + w + 1, y - 1, x - 1, y + tt, SSD1306_BLACK);
    } else if (tt < 0) {
        int d = -tt;
        if (isLeft) display.fillTriangle(x - 1, y - 1, x + w + 1, y - 1, x - 1, y + d, SSD1306_BLACK);
        else display.fillTriangle(x - 1, y - 1, x + w + 1, y - 1, x + w + 1, y + d, SSD1306_BLACK);
    }

    // Bottom cutout (for happy smile / laugh arcs)
    int bc = (int)roundf(botCurve);
    if (bc > 0) {
        display.fillRoundRect(x - 1, y + h - bc, w + 2, bc + 2, EYE_RADIUS, SSD1306_BLACK);
    }
}

int singleLineCaption(const char *txt, int targetLine, int yPos, bool draw) {
    const int maxChars = 20;
    int len = strlen(txt);
    int idx = 0;
    int lineNum = 0;
    char lineBuf[24];

    while (idx < len) {
        while (idx < len && txt[idx] == ' ') idx++;
        if (idx >= len) break;

        int take = len - idx;
        if (take > maxChars) {
            take = maxChars;
            int split = take;
            while (split > 0 && txt[idx + split] != ' ') split--;
            if (split > 0) take = split;
        }

        if (draw && lineNum == targetLine) {
            memcpy(lineBuf, txt + idx, take);
            lineBuf[take] = '\0';
            int xStart = (128 - take * 6) / 2;
            if (xStart < 10) xStart = 10;
            display.setTextSize(1);
            display.setTextColor(SSD1306_WHITE);
            display.setTextWrap(false);
            display.setCursor(xStart, yPos);
            display.print(lineBuf);
        }
        lineNum++;
        idx += take;
    }
    return lineNum;
}

void oledTask(void *param) {
    const float baseH = (float)BASE_EYE_H;
    RoboEyeRig cur = {0, 0, baseH, baseH, 0, 0, 0, 0};
    RoboEyeRig tgt = {0, 0, baseH, baseH, 0, 0, 0, 0};

    uint32_t nextIdleLookMs = millis() + 1000;
    uint32_t nextBlinkMs    = millis() + BLINK_MIN_MS;
    uint32_t blinkEndMs     = 0;

    uint32_t lastInteraction = millis();
    uint32_t speakStartMs    = millis();
    uint32_t moodHoldUntil   = 0;
    uint8_t  holdMood        = MOOD_NORMAL;
    uint8_t  prevState       = STATE_IDLE;

    static char localText[512] = "";
    int   totalLines = 0;
    float smoothMic  = 0.0f;
    bool  isDimmed   = false;

    while (true) {
        uint32_t now = millis();
        uint8_t state, mood;
        bool textUpdated;

        portENTER_CRITICAL(&uiMux);
        state = uiState;
        mood  = uiMood;
        textUpdated = uiTextChanged;
        if (textUpdated) {
            strncpy(localText, uiText, sizeof(localText) - 1);
            localText[sizeof(localText) - 1] = '\0';
            uiTextChanged = false;
        }
        portEXIT_CRITICAL(&uiMux);

        if (textUpdated) {
            speakStartMs = now;
            totalLines = singleLineCaption(localText, 0, 0, false);
            holdMood = mood;
        }

        float targetMic = (float)micLevel;
        if (targetMic > smoothMic) smoothMic = smoothMic * 0.40f + targetMic * 0.60f;
        else smoothMic = smoothMic * 0.85f + targetMic * 0.15f;

        if (state != STATE_IDLE) lastInteraction = now;
        if (prevState == STATE_SPEAKING && state == STATE_IDLE && holdMood != MOOD_NORMAL) {
            moodHoldUntil = now + 2200;
        }
        prevState = state;

        bool isSleeping = (state == STATE_IDLE && (now - lastInteraction > SLEEP_AFTER_MS));
        if (isSleeping && !isDimmed) {
            display.dim(true);
            isDimmed = true;
        } else if (!isSleeping && isDimmed) {
            display.dim(false);
            isDimmed = false;
        }

        if (!isSleeping && now > nextBlinkMs) {
            blinkEndMs  = now + 90;
            nextBlinkMs = now + random(BLINK_MIN_MS, BLINK_MAX_MS);
        }
        bool blinking = (!isSleeping && now < blinkEndMs);

        int shakeX  = 0;
        int bounceY = 0;
        uint8_t activeMood = (state == STATE_SPEAKING)
                                 ? mood
                                 : ((now < moodHoldUntil) ? holdMood : MOOD_NORMAL);

        if (state == STATE_LISTENING) {
            tgt.gazeX    = 0.0f;
            tgt.gazeY    = -1.0f;
            float openH  = baseH + (smoothMic * 0.05f);
            tgt.hL = openH;
            tgt.hR = openH;
            tgt.topL = 0; tgt.topR = 0;
            tgt.tiltTop = 0; tgt.botCurve = 0;

        } else if (state == STATE_THINKING) {
            // Curious upward glance (looking NE)
            tgt.gazeX    = (float)LOOK_RANGE_X * 0.8f;
            tgt.gazeY    = -(float)LOOK_RANGE_Y * 0.9f;
            tgt.hL       = baseH * 0.80f;
            tgt.hR       = baseH + (float)CURIOSITY_GROWTH; // Curiosity stretch!
            tgt.topL     = baseH * 0.20f;
            tgt.topR     = 0;
            tgt.tiltTop  = 0;
            tgt.botCurve = 0;

        } else if (state == STATE_SPEAKING || now < moodHoldUntil) {
            float talkPulse = (state == STATE_SPEAKING) ? (sinf(now * 0.025f) * 2.2f) : 0.0f;

            switch (activeMood) {
                case MOOD_HAPPY:
                    tgt = {0.0f, -1.0f, baseH + talkPulse, baseH + talkPulse, 0.0f, 0.0f, 0.0f, baseH * 0.55f};
                    bounceY = (int)(sinf(now * 0.022f) * 2.0f);
                    break;
                case MOOD_LAUGH:
                    tgt = {0.0f, -2.0f, baseH * 0.85f, baseH * 0.85f, 0.0f, 0.0f, 0.0f, baseH * 0.65f};
                    bounceY = (int)(sinf(now * 0.038f) * 3.0f);
                    break;
                case MOOD_COOL:
                    tgt = {6.0f, 0.0f, baseH * 0.80f, baseH + 3.0f, baseH * 0.35f, 0.0f, baseH * 0.15f, 0.0f};
                    break;
                case MOOD_ANNOYED:
                    tgt = {7.0f, -1.0f, baseH * 0.85f, baseH * 0.85f, baseH * 0.45f, baseH * 0.30f, 0.0f, 0.0f};
                    break;
                case MOOD_ANGRY:
                    tgt = {0.0f, 1.0f, baseH * 0.95f, baseH * 0.95f, 1.0f, 1.0f, baseH * 0.55f, 0.0f};
                    shakeX = ((now / 55) % 2 == 0) ? 1 : -1;
                    break;
                case MOOD_SAD:
                    tgt = {0.0f, 4.0f, baseH * 0.90f, baseH * 0.90f, 1.0f, 1.0f, -baseH * 0.45f, 0.0f};
                    break;
                default:
                    // Normal vocal resonance
                    tgt = {0.0f, -1.0f, baseH + talkPulse, baseH + talkPulse, 0.0f, 0.0f, 0.0f, 0.0f};
                    break;
            }

        } else {
            // IDLE: Always keep expressions NORMAL, active cardinal glances + curiosity!
            tgt.topL = 0; tgt.topR = 0;
            tgt.tiltTop = 0; tgt.botCurve = 0;

            uint32_t idleTime = now - lastInteraction;
            if (idleTime > SLEEP_AFTER_MS) {
                tgt.gazeX = 0; tgt.gazeY = 3.0f;
                tgt.hL = 3.0f; tgt.hR = 3.0f;
            } else {
                if (now > nextIdleLookMs) {
                    // Pick one of the 9 FluxGarage cardinal positions
                    int pos = random(0, 9);
                    switch (pos) {
                        case 0: // Center
                            tgt.gazeX = 0; tgt.gazeY = 0;
                            tgt.hL = baseH; tgt.hR = baseH;
                            break;
                        case 1: // West (Look Left) -> Left eye grows +8px!
                            tgt.gazeX = -LOOK_RANGE_X; tgt.gazeY = 0;
                            tgt.hL = baseH + CURIOSITY_GROWTH; tgt.hR = baseH;
                            break;
                        case 2: // East (Look Right) -> Right eye grows +8px!
                            tgt.gazeX = LOOK_RANGE_X; tgt.gazeY = 0;
                            tgt.hL = baseH; tgt.hR = baseH + CURIOSITY_GROWTH;
                            break;
                        case 3: // North (Look Up)
                            tgt.gazeX = 0; tgt.gazeY = -LOOK_RANGE_Y;
                            tgt.hL = baseH; tgt.hR = baseH;
                            break;
                        case 4: // South (Look Down)
                            tgt.gazeX = 0; tgt.gazeY = LOOK_RANGE_Y;
                            tgt.hL = baseH; tgt.hR = baseH;
                            break;
                        case 5: // North-West (Up-Left) -> Left eye grows!
                            tgt.gazeX = -LOOK_RANGE_X; tgt.gazeY = -LOOK_RANGE_Y;
                            tgt.hL = baseH + CURIOSITY_GROWTH; tgt.hR = baseH;
                            break;
                        case 6: // North-East (Up-Right) -> Right eye grows!
                            tgt.gazeX = LOOK_RANGE_X; tgt.gazeY = -LOOK_RANGE_Y;
                            tgt.hL = baseH; tgt.hR = baseH + CURIOSITY_GROWTH;
                            break;
                        case 7: // South-West (Down-Left) -> Left eye grows!
                            tgt.gazeX = -LOOK_RANGE_X; tgt.gazeY = LOOK_RANGE_Y;
                            tgt.hL = baseH + CURIOSITY_GROWTH; tgt.hR = baseH;
                            break;
                        case 8: // South-East (Down-Right) -> Right eye grows!
                            tgt.gazeX = LOOK_RANGE_X; tgt.gazeY = LOOK_RANGE_Y;
                            tgt.hL = baseH; tgt.hR = baseH + CURIOSITY_GROWTH;
                            break;
                    }

                    nextIdleLookMs = now + random(1500, 3200);
                }
            }
        }

        // Half-step kinetic interpolation
        cur.gazeX    = lerpf(cur.gazeX,    tgt.gazeX,    EYE_SMOOTHNESS);
        cur.gazeY    = lerpf(cur.gazeY,    tgt.gazeY,    EYE_SMOOTHNESS);
        cur.hL       = lerpf(cur.hL,       tgt.hL,       EYE_SMOOTHNESS);
        cur.hR       = lerpf(cur.hR,       tgt.hR,       EYE_SMOOTHNESS);
        cur.topL     = lerpf(cur.topL,     tgt.topL,     EYE_SMOOTHNESS);
        cur.topR     = lerpf(cur.topR,     tgt.topR,     EYE_SMOOTHNESS);
        cur.tiltTop  = lerpf(cur.tiltTop,  tgt.tiltTop,  EYE_SMOOTHNESS);
        cur.botCurve = lerpf(cur.botCurve, tgt.botCurve, EYE_SMOOTHNESS);

        display.clearDisplay();

        // 1. Draw Sleek Top HUD & Network Telemetry
        drawTopHUD(state);

        // 2. Render Sharp Square-ish Eyes
        int halfSpan = (EYE_SPACING / 2) + (BASE_EYE_W / 2);
        int cxL = 64 - halfSpan + (int)roundf(cur.gazeX) + shakeX;
        int cxR = 64 + halfSpan + (int)roundf(cur.gazeX) + shakeX;
        int cy  = EYE_CENTER_Y + (int)roundf(cur.gazeY) + bounceY;

        drawSquareEye(cxL, cy, BASE_EYE_W, (int)roundf(cur.hL), cur.topL, cur.tiltTop, cur.botCurve, true, blinking);
        drawSquareEye(cxR, cy, BASE_EYE_W, (int)roundf(cur.hR), cur.topR, cur.tiltTop, cur.botCurve, false, blinking);

        // 3. State-Specific Visuals at bottom
        if (state == STATE_LISTENING) {
            float amp = (smoothMic / 100.0f) * 12.0f;
            if (amp < 2.0f) amp = 2.0f;
            for (int i = 0; i < 9; i++) {
                float h = sinf(now * 0.02f + i * 0.7f) * amp;
                if (h < 0) h = -h;
                int barH = 2 + (int)h;
                display.drawFastVLine(64 + i * 4, 58 - barH / 2, barH, SSD1306_WHITE);
                display.drawFastVLine(64 - i * 4, 58 - barH / 2, barH, SSD1306_WHITE);
            }
        } 
        else if (state == STATE_THINKING) {
            float angle = now * 0.007f;
            int rx = 64 + (int)(cosf(angle) * 7.0f);
            int ry = 56 + (int)(sinf(angle) * 3.5f);
            display.drawCircle(64, 56, 4, SSD1306_WHITE);
            display.fillCircle(rx, ry, 2, SSD1306_WHITE);

            uint8_t hx = (now >> 6) & 0xFF;
            display.setTextSize(1);
            display.setCursor(85, 53);
            display.printf("0x%02X", hx);
        } 
        else if (state == STATE_SPEAKING && totalLines > 0) {
            display.drawFastHLine(14, 48, 100, SSD1306_WHITE);
            int lineIdx = ((now - speakStartMs) / CAPTION_MS) % totalLines;
            singleLineCaption(localText, lineIdx, 53, true);
        }

        display.display();
        vTaskDelay(pdMS_TO_TICKS(33)); // 30 FPS
    }
}

static bool isPushToTalking = false;

void triggerPushToTalkStart() {
    Serial.println("[PTT] Button PRESSED -> Listening...");

    size_t itemSize = 0;
    void *item = NULL;
    while ((item = xRingbufferReceiveUpTo(audioRingBuf, &itemSize, 0, 1024)) != NULL) {
        vRingbufferReturnItem(audioRingBuf, item);
    }
    flushSilence();
    isSpeaking = false;
    lastSpeakEndMs = 0; 

    portENTER_CRITICAL(&uiMux);
    uiState = STATE_LISTENING;
    pendingIdle = false;
    portEXIT_CRITICAL(&uiMux);


    if (udpSock >= 0) {
        PacketHeader stopHdr;
        stopHdr.seq    = 0;
        stopHdr.magic  = PROTOCOL_MAGIC;
        stopHdr.length = 0;
        stopHdr.type   = MSG_TYPE_STOP;
        sendto(udpSock, &stopHdr, sizeof(PacketHeader), 0,
               (struct sockaddr *)&serverAddr, sizeof(serverAddr));
    }
    isPushToTalking = true;
}

void triggerPushToTalkEnd() {
    if (!isPushToTalking) return;
    Serial.println("[PTT] Button RELEASED -> Processing utterance...");
    isPushToTalking = false;


    portENTER_CRITICAL(&uiMux);
    uiState = STATE_THINKING;
    portEXIT_CRITICAL(&uiMux);
    if (udpSock >= 0) {
        PacketHeader stopHdr;
        stopHdr.seq    = 0;
        stopHdr.magic  = PROTOCOL_MAGIC;
        stopHdr.length = 0;
        stopHdr.type   = MSG_TYPE_STOP;
        sendto(udpSock, &stopHdr, sizeof(PacketHeader), 0,
               (struct sockaddr *)&serverAddr, sizeof(serverAddr));
    }
}


void setup() {
    Serial.begin(115200);
    delay(500);
    randomSeed(esp_random());
    Serial.printf("\n=== DeskAI Unified Engine (Heap: %lu B) ===\n", (unsigned long)ESP.getFreeHeap());


    pinMode(BOOT_BTN, INPUT_PULLUP);

    Wire.begin(OLED_SDA, OLED_SCL);
    if (!display.begin(SSD1306_SWITCHCAPVCC, I2C_ADDRESS)) {
        Serial.println("SSD1306 allocation failed!");
        while (true) delay(1000);
    }
    Wire.setClock(400000);

    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(14, 28);
    display.print("[SYS] Booting Net...");
    display.display();

    audioRingBuf = xRingbufferCreate(RINGBUF_BYTES, RINGBUF_TYPE_BYTEBUF);
    if (!audioRingBuf || !initMicI2S() || !initAmpI2S()) {
        Serial.println("ERROR: RingBuf or I2S init failed!");
        while (true) delay(1000);
    }

    WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
        Serial.println("[WiFi] Lost connection! Auto-reconnecting...");
        WiFi.reconnect();
    }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);

    WiFi.mode(WIFI_STA);
    WiFi.setTxPower(WIFI_POWER_15dBm);
    WiFi.setSleep(false);
    WiFi.setAutoReconnect(true);
    WiFi.begin(WIFI_SSID, WIFI_PASS);

    Serial.printf("Connecting to Wi-Fi: %s", WIFI_SSID);
    while (WiFi.status() != WL_CONNECTED) {
        delay(300);
        Serial.print(".");
    }
    Serial.printf("\nConnected! ESP32 IP: %s\n", WiFi.localIP().toString().c_str());

    if (!initUDPSocket()) {
        Serial.println("ERROR: UDP socket init failed!");
        while (true) delay(1000);
    }

    xTaskCreatePinnedToCore(micTask,   "MicTask",   4096, NULL, 2, NULL, 0);
    xTaskCreatePinnedToCore(netRxTask, "NetRxTask", 4096, NULL, 3, NULL, 0);
    xTaskCreatePinnedToCore(oledTask,  "OledTask",  4096, NULL, 1, NULL, 0);
    xTaskCreatePinnedToCore(ampTask,   "AmpTask",   4096, NULL, 3, NULL, 1);
}

void loop() {
    static bool lastBtn = HIGH;
    bool btn = digitalRead(BOOT_BTN);

    if (btn == LOW && lastBtn == HIGH) {
        triggerPushToTalkStart();
    }
    else if (btn == HIGH && lastBtn == LOW) {
        triggerPushToTalkEnd();
    }

    lastBtn = btn;
    vTaskDelay(pdMS_TO_TICKS(20)); // 50 Hz poll rate
}