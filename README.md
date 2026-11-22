# INTRODUCTION

This server acts as a middleware between clients (an ESP32, a laptop simulator, or anything that speaks the packet format) and AI models for STT, LLM, and TTS.

- It receives UDP datagrams from the client on port `8000`.
- Stores incoming audio in a pooled buffer and reads the audio bytes in place.
- Filters out silence and detects when speech ends.
- Wraps the speech in a 44-byte WAV header and sends it to the STT provider.
- Sends the transcribed text to Gemini to get a spoken-style response and a mood tag.
- Strips the mood tag, sends the clean text to the TTS provider, and sends the mood + caption text to the client screen.
- Sends the generated audio back to the client over UDP to be played.

### Status
- [x] Packed binary UDP protocol (C + Go)
- [x] Go UDP server with buffer reuse (`sync.Pool`) and RMS silence detection
- [x] Swappable STT / TTS providers (Gemini, Groq/OpenAI cloud, or local `whisper.cpp` + Piper)
- [x] Laptop mic simulator (`cmd/mockmic`)
- [x] ESP32 I2S mic/speaker firmware (INMP441 + MAX98357A + 0.96" SSD1306 OLED with RoboEyes and mood reactions)
- [ ] Networked MP3 music streaming

---

# REQUIREMENTS
- Go
- `GEMINI_API_KEY` (plus `STT_API_KEY` / `TTS_API_KEY` if using Groq/cloud STT or TTS, or `whisper-server` + `piper` if running locally)
- `arecord` and `aplay` (for recording test audio and hearing replies on Linux)
- For ESP32: `arduino-cli` with the `esp32:esp32` core and libraries `Adafruit GFX Library`, `Adafruit SSD1306`, and `FluxGarage RoboEyes`

---

# USAGE

1. Load your environment variables (check `server/.env.example`) inside `server/`:
   ```bash
   cp .env.example .env
   ```

2. Start the server:
   ```bash
   set -a && source .env && set +a
   go run .
   ```

### TESTING WITHOUT HARDWARE
3. Record a 3-second `.wav` file inside `server/`:
   ```bash
   arecord -f S16_LE -r 16000 -c 1 -d 3 test_prompt.wav
   ```

4. In a second terminal inside `server/`, run the simulator to send the file you recorded:
   ```bash
   go run ./cmd/mockmic
   ```
   You will see your transcribed request and the AI response in the first terminal.

5. Play the response audio:
   ```bash
   aplay -t raw -r 24000 -f S16_LE -c 1 /tmp/reply.wav
   ```
   *(Use `-r 22050` if running Piper medium voices.)*

### RUNNING ON ESP32
1. Edit `firmware/secrets.h` (gitignored) with your 2.4 GHz Wi-Fi and laptop/server LAN IP:
   ```cpp
   #pragma once
   #define WIFI_SSID "your-2.4ghz-wifi"
   #define WIFI_PASS "your-wifi-password"
   #define SERVER_IP "your-laptop/server-lan-ip"
   ```
2. Make sure your firewall allows UDP port `8000` (for example on Arch with `nftables`: `sudo nft insert rule inet filter input udp dport 8000 accept`).
3. Compile and flash from the repo root:
   ```bash
   arduino-cli compile --fqbn esp32:esp32:esp32 firmware && arduino-cli upload -p /dev/ttyUSB0 --fqbn esp32:esp32:esp32 firmware
   ```
4. Wait for the startup chirp, then talk to it.

---

# HOW IT WORKS

```text
Client -> UDP (1033B) -> Go sync.Pool + VAD -> STT -> Gemini -> TTS -> UDP (Status + TTS Audio)
```

### 1. Packet Format (9-Byte Header + Payload)
Every UDP packet starts with a 9-byte little-endian header followed by up to `512` signed 16-bit PCM samples (`1024` bytes = `32 ms` of audio at `16 kHz`):

| Bytes | Field | Type | Description |
| :--- | :--- | :--- | :--- |
| `0..3` | `Seq` | `uint32` | Packet sequence number |
| `4..5` | `Magic` | `uint16` | Fixed protocol ID (`0xAE05`) |
| `6..7` | `Length` | `uint16` | Payload byte length (`0..1024`) |
| `8` | `MsgType` | `uint8` | `1` = Mic audio, `2` = TTS audio, `3` = Stop, `4` = Status/UI |
| `9..1032` | `Payload` | `int16[]` | Raw PCM audio bytes (or status bytes when `MsgType == 4`) |

For `MsgType == 4` (Status/UI):
- `payload[0]` = state (`0` = Listening, `1` = Thinking, `2` = Speaking, `3` = Idle)
- `payload[1]` = mood (`0` = Normal, `1` = Annoyed, `2` = Happy, `3` = Laugh, `4` = Angry, `5` = Sad, `6` = Cool)
- `payload[2:]` = reply text to show on the OLED (up to `1022` bytes)

Total max packet size is `1033` bytes, which stays under the `1472`-byte IPv4 UDP payload limit so packets don't get fragmented on Wi-Fi. In C (`firmware/protocol.h`), the struct uses `__attribute__((packed))` and `static_assert` so the compiler doesn't add padding bytes.

### 2. Server Design
- **Buffer reuse (`sync.Pool`):** Incoming packets are read into reused `1500`-byte `*[]byte` buffers instead of allocating a new slice per packet. `DecodePacket` slices the audio payload in place.
- **Non-blocking audio loop + turn taking:** The main loop reads UDP packets, checks loudness (`ComputeRMS`), and collects speech into `speechBuf`. Once it sees at least `4` loud packets and ~`1.44 s` of silence (`45` quiet packets), a `MsgTypeStop` packet, or `15` seconds of audio, it copies the utterance to a `1`-slot `utterances` channel and flips `aiBusy` (`atomic.Bool`). While `aiBusy` is true, the main loop keeps draining incoming UDP packets and discards them so old audio doesn't pile up in the OS buffer.
- **Swappable AI providers:** `ai.Pipeline` talks to `STTProvider` and `TTSProvider` interfaces (`server/ai/provider.go`), so switching between all-Gemini, cloud Whisper/TTS, or local `whisper.cpp` + Piper only takes changing `.env`.
- **Mood parsing:** Gemini prefixes each response with one of 7 mood tags (`[NORMAL]`, `[ANNOYED]`, `[HAPPY]`, `[LAUGH]`, `[ANGRY]`, `[SAD]`, `[COOL]`). `pipeline.go` parses the tag for the OLED and strips it before TTS so the speaker only reads the actual reply.
- **Paced reply streaming:** `aiWorker` sends TTS audio back to the sender's UDP address in `1024`-byte chunks spaced `15 ms` apart so the ESP32's small socket buffer isn't overwhelmed.

### 3. ESP32 Firmware Design
- **Dual-core FreeRTOS split:** Core 0 handles Wi-Fi/UDP (`MicTask`, `NetRxTask`) and the OLED (`OledTask`), while Core 1 is dedicated to audio playback (`AmpTask`) so Wi-Fi traffic doesn't stutter the speaker.
- **I2S audio:** `I2S_NUM_0` reads 32-bit stereo from the INMP441, takes the Left channel (`L/R` tied to `GND`), and shifts `>> 16` into 16-bit PCM. `I2S_NUM_1` drives the MAX98357A at `24 kHz` 16-bit stereo (duplicating mono to L+R) with `auto_clear = true` to prevent buzzing after playback stops.
- **Jitter buffer + echo mute:** Incoming TTS packets go into a `64 KB` FreeRTOS ring buffer with a `256 ms` pre-buffer before playback starts. `MicTask` pauses uploading while the speaker is playing (plus a `500 ms` cooldown) so it doesn't hear itself.
- **OLED UI (`0.96"` SSD1306):** Uses `FluxGarage_RoboEyes` when idle (auto-blinks, looks around, gets tired and sleeps when ignored), switches to a live mic wave when listening, a thinking face with a thought cloud while waiting on the server, and full-size mood faces with one-line captions underneath when speaking.

---

# HARDWARE WIRING (ESP32)

| Module | Pins | Power |
| :--- | :--- | :--- |
| **INMP441** (I2S Mic) | `SCK=33`, `WS=32`, `SD=35`, `L/R=GND` | `3.3V` |
| **MAX98357A** (I2S Amp) | `BCLK=26`, `LRC=25`, `DIN=27` | `5V / VIN` |
| **SSD1306** (0.96" I2C OLED) | `SDA=21`, `SCL=22` | `3.3V` |

---

# PROJECT STRUCTURE

```text
deskai/
├── .gitignore
├── README.md
├── firmware/
│   ├── firmware.ino          # ESP32 FreeRTOS audio + UDP + OLED firmware
│   ├── protocol.h            # Packed C structs matching Go wire format
│   └── secrets.h             # Local Wi-Fi & server IP (gitignored)
└── server/
    ├── .env.example
    ├── go.mod
    ├── main.go               # UDP listener, sync.Pool, VAD, aiWorker, sendTTS
    ├── protocol/protocol.go  # EncodePacket / DecodePacket + state/mood constants
    ├── audio/wav.go          # ComputeRMS and 44-byte WAV header builder
    ├── ai/
    │   ├── provider.go       # STTProvider / TTSProvider interfaces & env factory
    │   ├── pipeline.go       # STT -> Gemini -> mood parse -> TTS orchestration
    │   ├── gemini.go         # Gemini LLM client
    │   ├── stt_gemini.go     # Gemini STT
    │   ├── stt_whisper.go    # Whisper HTTP client (local whisper.cpp & Groq/OpenAI)
    │   ├── tts_gemini.go     # Gemini TTS
    │   ├── tts_cloud.go      # Cloud TTS HTTP client
    │   └── tts_piper.go      # Local Piper subprocess TTS
    └── cmd/mockmic/main.go   # Local WAV-over-UDP client simulator
```
```
