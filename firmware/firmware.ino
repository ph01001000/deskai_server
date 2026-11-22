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
// BOT LOOK & PERSONALITY TUNING (Edit these to style your bot!)
// =========================================================
static const int      EYE_WIDTH         = 32;    // Eye width in px (try 22..38)
static const int      EYE_HEIGHT        = 24;    // Eye height in px (try 16..30)
static const int      EYE_SPACING       = 18;    // Gap between eyes in px (try 10..26)
static const int      EYE_RADIUS        = 6;     // Corner roundness: 1=sharp cyber, 6=soft, 12=cute round
static const int      EYE_CENTER_Y      = 24;    // Vertical eye center (lower number = higher up)
static const float    EYE_SMOOTHNESS    = 0.24f; // Morph speed: 0.12=dreamy, 0.24=smooth, 0.45=snappy
static const int      LOOK_RANGE_X      = 11;    // How far left/right eyes glance when idle (px)
static const int      LOOK_RANGE_Y      = 4;     // How far up/down eyes glance when idle (px)
static const float    CURIOSITY_SCALE   = 2.5f;  // 3D depth stretch on side glances (0=flat, 4=strong)
static const bool     ENABLE_MOUTH      = false; // true = minimal talking bar below eyes, false = pure eyes
static const bool     INVERT_DISPLAY    = false; // true = black eyes on glowing white screen
static const uint32_t BLINK_MIN_MS      = 2400;  // Min time between blinks (ms)
static const uint32_t BLINK_MAX_MS      = 5200;  // Max time between blinks (ms)
static const uint32_t TIRED_AFTER_MS    = 25000; // Start getting sleepy after 25s idle
static const uint32_t SLEEP_AFTER_MS    = 45000; // Fall asleep after 45s idle
static const uint32_t CAPTION_MS        = 1300;  // How long each 1-line caption stays visible (ms)
static const float    PLAYBACK_GAIN     = 0.55f; // Speaker volume 0.10..1.00 (lower if USB browns out)
// =========================================================

static const uint16_t SERVER_PORT      = 8000;
static const uint16_t LOCAL_UDP_PORT   = 8001;
static const uint16_t PROTOCOL_MAGIC   = 0xAE05;
static const uint8_t  MSG_TYPE_MIC     = 1;
static const uint8_t  MSG_TYPE_TTS     = 2;
static const uint8_t  MSG_TYPE_STATUS  = 4;

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

static const uint32_t MIC_SAMPLE_RATE   = 16000;
static const uint32_t AMP_SAMPLE_RATE   = 24000;
static const size_t   MIC_SAMPLES       = 512;
static const size_t   RINGBUF_BYTES     = 32768; // ~680 ms jitter buffer @ 24 kHz (low DRAM footprint)
static const size_t   PREBUF_WATERMARK  = 10240; // ~213 ms pre-buffer before playing

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

static i2s_chan_handle_t rx_handle      = NULL;
static i2s_chan_handle_t tx_handle      = NULL;
static RingbufHandle_t   audioRingBuf   = NULL;

static int                udpSock       = -1;
static struct sockaddr_in serverAddr    = {};

static volatile bool     isSpeaking     = false;
static volatile uint32_t lastSpeakEndMs = 0;
static volatile uint8_t  micLevel       = 0;

static portMUX_TYPE     uiMux           = portMUX_INITIALIZER_UNLOCKED;
static volatile uint8_t uiState         = STATE_IDLE;
static volatile uint8_t uiMood          = MOOD_NORMAL;
static volatile bool    pendingIdle     = false;
static char             uiText[512]     = "";
static volatile bool    uiTextChanged   = false;

struct EyeRig {
    float gazeX;
    float gazeY;
    float hL;
    float hR;
    float topL;
    float topR;
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
    std_cfg.gpio_cfg.invert_flags.mclk_inv = false;
    std_cfg.gpio_cfg.invert_flags.bclk_inv = false;
    std_cfg.gpio_cfg.invert_flags.ws_inv   = false;

    if (i2s_channel_init_std_mode(tx_handle, &std_cfg) != ESP_OK) return false;
    return i2s_channel_enable(tx_handle) == ESP_OK;
}

void flushSilence() {
    static int16_t zeros[512] = {0};
    for (int i = 0; i < 8; i++) {
        size_t written = 0;
        i2s_channel_write(tx_handle, zeros, sizeof(zeros), &written, pdMS_TO_TICKS(50));
    }
}

void playReadyChirp() {
    static int16_t buf[512];
    for (int c = 0; c < 10; c++) {
        for (size_t i = 0; i < 256; i++) {
            float t = (float)(c * 256 + i) / (float)AMP_SAMPLE_RATE;
            int16_t s = (int16_t)(1600.0f * sinf(2.0f * PI * 660.0f * t));
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

        sendto(udpSock, &txPkt, sizeof(AudioPacket), 0,
               (struct sockaddr *)&serverAddr, sizeof(serverAddr));
    }
}

void netRxTask(void *param) {
    static AudioPacket rxPkt;

    while (true) {
        int n = recvfrom(udpSock, &rxPkt, sizeof(rxPkt), 0, NULL, NULL);
        if (n < (int)sizeof(PacketHeader)) continue;
        if (rxPkt.header.magic != PROTOCOL_MAGIC) continue;
        if (rxPkt.header.length == 0 || rxPkt.header.length > 1024) continue;
        if (n < (int)(sizeof(PacketHeader) + rxPkt.header.length)) continue;

        if (rxPkt.header.type == MSG_TYPE_TTS) {
            xRingbufferSend(audioRingBuf, rxPkt.samples, rxPkt.header.length, 0);
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

            Serial.printf("[UI] state=%u mood=%u textLen=%u\n", newState, newMood, (unsigned)textLen);
        }
    }
}

void ampTask(void *param) {
    static int16_t stereoOut[512 * 2];
    bool playing = false;
    uint32_t firstByteTime = 0;

    vTaskDelay(pdMS_TO_TICKS(300)); // Let Wi-Fi settle before startup chirp
    playReadyChirp();
    Serial.println("[AmpTask] Speaker ready.");

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

int singleLineCaption(const char *txt, int targetLine, int yPos, bool draw) {
    const int maxChars = 21;
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
            if (xStart < 1) xStart = 1;
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

void drawOneEye(int cx, int cy, int w, int h, float topCut, float tiltTop, float botCurve, bool isLeft) {
    if (h < 3) h = 3;
    int r = EYE_RADIUS;
    if (r > h / 2) r = h / 2;
    if (r > w / 2) r = w / 2;

    int x = cx - w / 2;
    int y = cy - h / 2;

    display.fillRoundRect(x, y, w, h, r, SSD1306_WHITE);

    int tc = (int)roundf(topCut);
    if (tc > 0) {
        display.fillRect(x - 1, y - 1, w + 2, tc + 1, SSD1306_BLACK);
    }

    int tt = (int)roundf(tiltTop);
    if (tt > 0) {
        if (isLeft) {
            display.fillTriangle(x - 1, y - 1, x + w + 1, y - 1, x + w + 1, y + tt, SSD1306_BLACK);
        } else {
            display.fillTriangle(x - 1, y - 1, x + w + 1, y - 1, x - 1, y + tt, SSD1306_BLACK);
        }
    } else if (tt < 0) {
        int d = -tt;
        if (isLeft) {
            display.fillTriangle(x - 1, y - 1, x + w + 1, y - 1, x - 1, y + d, SSD1306_BLACK);
        } else {
            display.fillTriangle(x - 1, y - 1, x + w + 1, y - 1, x + w + 1, y + d, SSD1306_BLACK);
        }
    }

    int bc = (int)roundf(botCurve);
    if (bc > 0) {
        int rad = (w * 3) / 4;
        display.fillCircle(cx, (y + h) + rad - bc, rad, SSD1306_BLACK);
    }
}

void oledTask(void *param) {
    const float baseH = (float)EYE_HEIGHT;
    EyeRig cur = {0, 0, baseH, baseH, 0, 0, 0, 0};
    EyeRig tgt = {0, 0, baseH, baseH, 0, 0, 0, 0};

    uint32_t nextIdleLookMs = millis() + 2000;
    float idleGazeX = 0, idleGazeY = 0;

    uint32_t nextBlinkMs = millis() + BLINK_MIN_MS;
    uint32_t blinkEndMs  = 0;

    uint32_t lastInteraction = millis();
    uint32_t speakStartMs    = millis();
    uint32_t moodHoldUntil   = 0;
    uint8_t  holdMood        = MOOD_NORMAL;
    uint8_t  prevState       = STATE_IDLE;

    static char localText[512] = "";
    int   totalLines = 0;
    float smoothMic  = 0.0f;

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

        if (state != STATE_IDLE) {
            lastInteraction = now;
        }
        if (prevState == STATE_SPEAKING && state == STATE_IDLE && holdMood != MOOD_NORMAL) {
            moodHoldUntil = now + 2200;
        }
        prevState = state;

        bool isSleeping = (state == STATE_IDLE && (now - lastInteraction > SLEEP_AFTER_MS));
        if (!isSleeping && now > nextBlinkMs) {
            blinkEndMs  = now + 115;
            nextBlinkMs = now + random(BLINK_MIN_MS, BLINK_MAX_MS);
        }
        bool blinking = (!isSleeping && now < blinkEndMs);

        int shakeX  = 0;
        int bounceY = 0;
        uint8_t activeMood = (state == STATE_SPEAKING)
                                 ? mood
                                 : ((now < moodHoldUntil) ? holdMood : MOOD_NORMAL);

        if (state == STATE_LISTENING) {
            tgt.gazeX    = sinf(now * 0.002f) * 2.5f;
            tgt.gazeY    = -2.0f;
            float openH  = baseH + (smoothMic * 0.045f);
            tgt.hL       = openH;
            tgt.hR       = openH;
            tgt.topL     = 0;
            tgt.topR     = 0;
            tgt.tiltTop  = 0;
            tgt.botCurve = 0;

        } else if (state == STATE_THINKING) {
            tgt.gazeX    = (float)LOOK_RANGE_X * 0.8f + sinf(now * 0.0025f) * 3.0f;
            tgt.gazeY    = -6.0f;
            tgt.hL       = baseH * 0.80f;
            tgt.hR       = baseH * 1.05f;
            tgt.topL     = baseH * 0.25f;
            tgt.topR     = baseH * 0.05f;
            tgt.tiltTop  = 0;
            tgt.botCurve = 0;

        } else if (state == STATE_SPEAKING || now < moodHoldUntil) {
            float talkPulse = (state == STATE_SPEAKING) ? (sinf(now * 0.020f) * 1.5f) : 0.0f;

            switch (activeMood) {
                case MOOD_ANNOYED:
                    tgt = {8.0f, -1.0f, baseH * 0.92f, baseH * 0.92f, baseH * 0.44f, baseH * 0.44f, 0.0f, 0.0f};
                    break;
                case MOOD_HAPPY:
                    tgt = {0.0f, -1.0f, baseH + talkPulse, baseH + talkPulse, 0.0f, 0.0f, 0.0f, baseH * 0.58f};
                    break;
                case MOOD_LAUGH:
                    tgt = {0.0f, -2.0f, baseH, baseH, 0.0f, 0.0f, 0.0f, baseH * 0.67f};
                    bounceY = (int)(sinf(now * 0.035f) * 3.0f);
                    break;
                case MOOD_ANGRY:
                    tgt = {0.0f, 1.0f, baseH * 0.96f, baseH * 0.96f, 1.0f, 1.0f, baseH * 0.56f, 0.0f};
                    shakeX = ((now / 55) % 2 == 0) ? 1 : -1;
                    break;
                case MOOD_SAD:
                    tgt = {0.0f, 4.0f, baseH * 0.96f, baseH * 0.96f, 1.0f, 1.0f, -baseH * 0.52f, 0.0f};
                    break;
                case MOOD_COOL:
                    tgt = {5.0f, 0.0f, baseH * 0.96f, baseH * 0.96f, baseH * 0.40f, baseH * 0.25f, baseH * 0.08f, 0.0f};
                    break;
                default: // MOOD_NORMAL
                    tgt = {0.0f, -1.0f, baseH + talkPulse, baseH + talkPulse, 0.0f, 0.0f, 0.0f, 0.0f};
                    break;
            }

        } else {
            uint32_t idleTime = now - lastInteraction;
            if (idleTime > SLEEP_AFTER_MS) {
                tgt = {0.0f, 4.0f, 3.0f, 3.0f, 0.0f, 0.0f, 0.0f, 0.0f};
            } else {
                if (now > nextIdleLookMs) {
                    idleGazeX = (float)random(-LOOK_RANGE_X, LOOK_RANGE_X + 1);
                    idleGazeY = (float)random(-LOOK_RANGE_Y, LOOK_RANGE_Y + 1);
                    nextIdleLookMs = now + random(1500, 3800);
                }
                float curL = (idleGazeX < -3) ? CURIOSITY_SCALE : 0.0f;
                float curR = (idleGazeX >  3) ? CURIOSITY_SCALE : 0.0f;
                float tiredDrop = (idleTime > TIRED_AFTER_MS) ? (baseH * 0.38f) : 0.0f;

                tgt = {idleGazeX, idleGazeY, baseH + curL, baseH + curR, tiredDrop, tiredDrop, 0.0f, 0.0f};
            }
        }

        cur.gazeX    = lerpf(cur.gazeX,    tgt.gazeX,    EYE_SMOOTHNESS);
        cur.gazeY    = lerpf(cur.gazeY,    tgt.gazeY,    EYE_SMOOTHNESS);
        cur.hL       = lerpf(cur.hL,       tgt.hL,       EYE_SMOOTHNESS);
        cur.hR       = lerpf(cur.hR,       tgt.hR,       EYE_SMOOTHNESS);
        cur.topL     = lerpf(cur.topL,     tgt.topL,     EYE_SMOOTHNESS);
        cur.topR     = lerpf(cur.topR,     tgt.topR,     EYE_SMOOTHNESS);
        cur.tiltTop  = lerpf(cur.tiltTop,  tgt.tiltTop,  EYE_SMOOTHNESS);
        cur.botCurve = lerpf(cur.botCurve, tgt.botCurve, EYE_SMOOTHNESS);

        float drawHL = blinking ? 3.0f : cur.hL;
        float drawHR = blinking ? 3.0f : cur.hR;
        float drawTL = blinking ? 0.0f : cur.topL;
        float drawTR = blinking ? 0.0f : cur.topR;
        float drawTT = blinking ? 0.0f : cur.tiltTop;
        float drawBC = blinking ? 0.0f : cur.botCurve;

        display.clearDisplay();

        int halfSpan = (EYE_SPACING / 2) + (EYE_WIDTH / 2);
        int cxL = 64 - halfSpan + (int)roundf(cur.gazeX) + shakeX;
        int cxR = 64 + halfSpan + (int)roundf(cur.gazeX) + shakeX;
        int cy  = EYE_CENTER_Y + (int)roundf(cur.gazeY) + bounceY;

        drawOneEye(cxL, cy, EYE_WIDTH, (int)roundf(drawHL), drawTL, drawTT, drawBC, true);
        drawOneEye(cxR, cy, EYE_WIDTH, (int)roundf(drawHR), drawTR, drawTT, drawBC, false);

        if (ENABLE_MOUTH && state == STATE_SPEAKING) {
            int mouthH = 2 + (int)((sinf(now * 0.028f) + 1.0f) * 2.0f);
            display.fillRoundRect(56, EYE_CENTER_Y + EYE_HEIGHT / 2 + 5, 16, mouthH, 2, SSD1306_WHITE);
        }

        if (state == STATE_LISTENING) {
            float amp = 2.0f + (smoothMic / 100.0f) * 6.0f;
            float phase = now * 0.016f;
            int prevX = 18, prevY = 55;
            for (int x = 18; x <= 110; x += 3) {
                float norm = (float)(x - 18) / 92.0f;
                float env  = sinf(norm * PI);
                int y = 55 + (int)(sinf(norm * 13.0f - phase) * amp * env);
                if (x > 18) {
                    display.drawLine(prevX, prevY, x, y, SSD1306_WHITE);
                    display.drawLine(prevX, prevY + 1, x, y + 1, SSD1306_WHITE);
                }
                prevX = x;
                prevY = y;
            }
        } else if (state == STATE_THINKING) {
            for (int d = 0; d < 3; d++) {
                float p = (sinf(now * 0.008f - d * 1.1f) + 1.0f) * 0.5f;
                int r = (p > 0.65f) ? 3 : 2;
                int dy = (int)(-p * 3.0f);
                display.fillCircle(52 + d * 12, 57 + dy, r, SSD1306_WHITE);
            }
        } else if (state == STATE_SPEAKING && totalLines > 0) {
            int lineIdx = ((now - speakStartMs) / CAPTION_MS) % totalLines;
            singleLineCaption(localText, lineIdx, 54, true);
        }

        display.display();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void setup() {
    Serial.begin(115200);
    delay(500);
    randomSeed(esp_random());
    Serial.printf("\n=== DeskAI Unified Eye Engine (Heap: %lu B) ===\n", (unsigned long)ESP.getFreeHeap());

    Wire.begin(OLED_SDA, OLED_SCL);
    if (!display.begin(SSD1306_SWITCHCAPVCC, I2C_ADDRESS)) {
        Serial.println("SSD1306 allocation failed!");
        while (true) delay(1000);
    }
    Wire.setClock(400000);
    display.invertDisplay(INVERT_DISPLAY);

    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(16, 28);
    display.print("Connecting WiFi...");
    display.display();

    audioRingBuf = xRingbufferCreate(RINGBUF_BYTES, RINGBUF_TYPE_BYTEBUF);
    if (!audioRingBuf || !initMicI2S() || !initAmpI2S()) {
        Serial.println("ERROR: RingBuf or I2S init failed!");
        while (true) delay(1000);
    }

    WiFi.mode(WIFI_STA);
    WiFi.setTxPower(WIFI_POWER_15dBm); // Prevents USB brownout spikes
    WiFi.setSleep(false);
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
    vTaskDelay(pdMS_TO_TICKS(1000));
}