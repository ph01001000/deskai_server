# INTRODUCTION

This server acts as a middleware between clients (an ESP32, a laptop simulator, or anything that speaks the packet format) and AI models for STT, LLM, and TTS.

- It receives UDP datagrams from the client on port `8000`.
- Stores incoming audio in a pooled buffer and reads audio bytes in place.
- Filters out silence using RMS energy detection with a rolling pre-roll buffer to keep word onsets from getting clipped.
- Wraps the speech in a 44-byte WAV header and sends it to the STT provider.
- Sends the transcribed text to Gemini to get a spoken-style response and a mood/music tag.
- Strips the mood tag, sends clean text to TTS, and sends the mood + caption text to the client OLED.
- Streams generated speech audio back to the client over UDP.
- Streams local MP3/FLAC tracks decoded in real-time via `ffmpeg` when asked for music or a specific vibe.

### Status
- [x] Packed binary UDP protocol (C + Go)
- [x] Go UDP server with buffer reuse (`sync.Pool`) and RMS silence detection
- [x] VAD pre-roll buffer to prevent chopped speech onsets
- [x] Swappable STT / TTS providers (Gemini, Groq/OpenAI cloud, or local `whisper.cpp` + Piper)
- [x] Dynamic I2S sample rate switching over the air (16k / 22.05k / 24k / 44.1k)
- [x] Laptop mic simulator (`cmd/mockmic`)
- [x] ESP32 I2S mic/speaker firmware (INMP441 + MAX98357A + 0.96" SSD1306 OLED)
- [x] Sharp 3D procedural eye engine with live Wi-Fi telemetry and curiosity scaling
- [x] Networked MP3/FLAC music streaming (real-time ffmpeg decode + vibe-matching)
- [x] Hardware push-to-talk & music kill switch (built-in BOOT button on GPIO 0)

---

# REQUIREMENTS
- Go (1.22+)
- `ffmpeg` (required for real-time music streaming)
- `GEMINI_API_KEY` (the server will prompt for this on first run if missing)
- `arecord` and `aplay` (optional, for recording test audio and hearing replies locally on Linux)
- For ESP32: `arduino-cli` (or Arduino IDE) with the `esp32:esp32` core and libraries:
  - `Adafruit GFX Library`
  - `Adafruit SSD1306`
  - *(Note: No external RoboEyes library needed; procedural eye rendering is built into the sketch).*

---

# USAGE

1. Start the server:
   ```bash
   cd server
   go run .
   ```
   *If running for the first time without a `.env` file, the server will prompt you to paste your Gemini API key and generate `.env` automatically.*

2. To use the music player, drop songs or categorized artist folders into `server/music/`:
   ```text
   server/music/
   ├── Metallica/
   │   └── Enter_Sandman.flac
   ├── Linkin_Park/
   │   └── In_The_End.mp3
   └── lofi_chill.mp3
   ```

### TESTING WITHOUT HARDWARE
3. Record a 3-second `.wav` file inside `server/`:
   ```bash
   arecord -f S16_LE -r 16000 -c 1 -d 3 test_prompt.wav
   ```

4. In a second terminal inside `server/`, run the simulator to send the file:
   ```bash
   go run ./cmd/mockmic
   ```

5. Play the generated response audio:
   ```bash
   aplay -t raw -r 24000 -f S16_LE -c 1 /tmp/reply.wav
   ```

### RUNNING ON ESP32
1. Edit `firmware/secrets.h` (gitignored) with your 2.4 GHz Wi-Fi and server local LAN IP:
   ```cpp
   #pragma once
   #define WIFI_SSID "your-2.4ghz-wifi"
   #define WIFI_PASS "your-wifi-password"
   #define SERVER_IP "your-laptop/server-lan-ip"
   ```
2. Make sure your firewall allows UDP port `8000` (e.g. on Arch with `nftables`: `sudo nft insert rule inet filter input udp dport 8000 accept`).
3. Compile and flash:
   ```bash
   arduino-cli compile --fqbn esp32:esp32:esp32 firmware && arduino-cli upload -p /dev/ttyUSB0 --fqbn esp32:esp32:esp32 firmware
   ```
4. Wait for the startup chirp, then talk to it.

---

# HOW IT WORKS

```text
Client (ESP32) -> UDP (1033B) -> Go sync.Pool + VAD -> STT -> Gemini -> TTS / ffmpeg -> UDP (Status + Audio)
```

### 1. Packet Format (9-Byte Header + Payload)
Every UDP packet starts with a 9-byte little-endian header followed by up to `512` signed 16-bit PCM samples (`1024` bytes = `32 ms` of audio at `16 kHz`):

| Bytes | Field | Type | Description |
| :--- | :--- | :--- | :--- |
| `0..3` | `Seq` | `uint32` | Packet sequence number |
| `4..5` | `Magic` | `uint16` | Fixed protocol ID (`0xAE05`) |
| `6..7` | `Length` | `uint16` | Payload byte length (`0..1024`) |
| `8` | `MsgType` | `uint8` | `1` = Mic, `2` = TTS/Audio, `3` = Stop/Interrupt, `4` = Status/UI, `5` = Audio Config |
| `9..1032` | `Payload` | `int16[]` | Raw PCM audio bytes (or status/config bytes) |

- **`MsgType == 4` (Status/UI):**
  - `payload[0]` = state (`0` = Listening, `1` = Thinking, `2` = Speaking, `3` = Idle)
  - `payload[1]` = mood (`0` = Normal, `1` = Annoyed, `2` = Happy, `3` = Laugh, `4` = Angry, `5` = Sad, `6` = Cool, `7` = Music)
  - `payload[2:]` = text caption shown on the OLED (up to `1022` bytes)

- **`MsgType == 5` (Audio Config):**
  - `payload[0..3]` = `uint32` target sample rate (`16000`, `22050`, `24000`, `44100`). Sent before audio streams so the ESP32 can reconfigure its I2S hardware clock without restarting.

Total max packet size is `1033` bytes, staying well below the standard `1472`-byte IPv4 UDP payload limit to avoid Wi-Fi packet fragmentation. The struct uses `__attribute__((packed))` and `static_assert` to prevent compiler padding.

### 2. Server Design
- **Buffer reuse (`sync.Pool`):** Packets are read into reused `1500`-byte buffers. `DecodePacket` slices the audio payload in place without allocating new memory per frame.
- **VAD Pre-roll:** Audio volume is tracked via RMS. While idle, the server retains a rolling 4-packet history (~128 ms) so the opening syllable of a sentence isn't clipped when speech crosses the threshold.
- **Smart DJ / Music Streaming:** `GetAvailableTracks` recursively scans `server/music/` (including artist/genre subdirectories). Gemini picks the best track based on user request or vibe (e.g. *"play something chill"* vs *"play metal"*), returns `[MUSIC:folder/song.ext]`, and `StreamMP3` feeds real-time chunks via `ffmpeg -re` over UDP.
- **Push-to-Talk / Hardware Interrupt:** If the client sends `MsgTypeStop` (via the BOOT button), active music or speech is terminated immediately on the server, and the mic is opened for voice commands without waiting for silence timeouts.

### 3. ESP32 Firmware Design
- **Dual-core FreeRTOS split:** Core 0 handles Wi-Fi, UDP networking (`MicTask`, `NetRxTask`), and OLED rendering (`OledTask`). Core 1 is dedicated to I2S playback (`AmpTask`) to keep speaker output jitter-free.
- **Dynamic Sample Rate Switching:** Supports run-time I2S clock reconfiguration via `setAmpSampleRate()`.
- **Jitter Buffer & Sequence Gating:** Packets enter a `32 KB` FreeRTOS ring buffer. `NetRxTask` validates packet sequence numbers, dropping stale or out-of-order packets.
- **Echo Suppression:** `MicTask` mutes recording while audio is actively playing through the speaker, plus a cooldown period.
- **Hardware Push-to-Talk:** Pressing the built-in `BOOT` button (GPIO 0) flushes the local ring buffer immediately and notifies the server to stop playback and open the mic.
- **OLED UI (128x64 SSD1306):**
  - Top HUD with live Wi-Fi signal strength bars and telemetry tags (`[SYS//OK]`, `[REC//AUD]`, etc.).
  - Sharp 28x20 square-style eyes with 2px corner radius.
  - 9-cardinal direction idle glancing with +8px outer-eye stretch (curiosity scaling).
  - Procedural spectrum visualizer during listening, gyro telemetry during thinking, and text captions while speaking.
  - Auto-dimming on deep idle to protect against OLED burn-in.

---

# HARDWARE WIRING (ESP32)

| Module | Pins | Power |
| :--- | :--- | :--- |
| **INMP441** (I2S Mic) | `SCK=33`, `WS=32`, `SD=35`, `L/R=GND` | `3.3V` |
| **MAX98357A** (I2S Amp) | `BCLK=26`, `LRC=25`, `DIN=27` | `5V / VIN` |
| **SSD1306** (0.96" I2C OLED) | `SDA=21`, `SCL=22` | `3.3V` |
| **BOOT Button** (Push-to-Talk / Stop) | `GPIO 0` (Built onto dev board) | Internal Pull-Up |

---

# PROJECT STRUCTURE

```text
deskai/
├── .gitignore
├── README.md
├── firmware/
│   ├── firmware.ino          # ESP32 FreeRTOS audio + UDP + procedural OLED firmware
│   ├── protocol.h            # Packed C structs matching Go wire format
│   └── secrets.h             # Local Wi-Fi & server IP (gitignored)
└── server/
    ├── .env.example
    ├── config.go             # First-run interactive .env setup wizard
    ├── go.mod
    ├── main.go               # UDP loop, sync.Pool, VAD, aiWorker, streamMusic, PTT
    ├── music/                # Audio library for streaming (.mp3, .flac, etc.)
    ├── protocol/protocol.go  # Binary packet encode/decode + message constants
    ├── audio/wav.go          # RMS calculator and 44-byte WAV header builder
    ├── ai/
    │   ├── provider.go       # STTProvider / TTSProvider interfaces & factory
    │   ├── pipeline.go       # Speech -> Gemini -> track selection -> TTS pipeline
    │   ├── gemini.go         # Gemini LLM client with vibe-matching music prompt
    │   ├── music.go          # Recursive audio scanner + real-time ffmpeg streamer
    │   ├── stt_gemini.go     # Gemini STT
    │   ├── stt_whisper.go    # Whisper HTTP client (local whisper.cpp & cloud)
    │   ├── tts_gemini.go     # Gemini TTS
    │   ├── tts_cloud.go      # Cloud TTS HTTP client
    │   └── tts_piper.go      # Local Piper subprocess TTS
    └── cmd/mockmic/main.go   # Local WAV-over-UDP client simulator
```
