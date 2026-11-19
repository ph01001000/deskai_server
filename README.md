# INTRODUCTION

This server acts as a middleware between clients (an ESP32, a laptop simulator, or anything that speaks the packet format) and AI models for STT, LLM, and TTS.

- It receives UDP datagrams from the client on port `8000`.
- Stores incoming audio in a pooled buffer and reads the audio bytes in place.
- Filters out silence and detects when speech ends.
- Wraps the speech in a 44-byte WAV header and sends it to the STT provider.
- Sends the transcribed text to Gemini to get a spoken-style response.
- Sends the response to the TTS provider.
- Sends the generated audio back to the client over UDP to be played.

### Status
- [x] Packed binary UDP protocol (C + Go)
- [x] Go UDP server with buffer reuse (`sync.Pool`) and RMS silence detection
- [x] Swappable STT / TTS providers (Gemini, Groq/OpenAI cloud, or local `whisper.cpp` + Piper)
- [x] Laptop mic simulator (`cmd/mockmic`)
- [ ] ESP32 I2S mic/speaker firmware (waiting on hardware)
- [ ] Networked MP3 music streaming

---

# REQUIREMENTS
- Go
- `GEMINI_API_KEY` (plus `STT_API_KEY` / `TTS_API_KEY` if using Groq/cloud STT or TTS, or `whisper-server` + `piper` if running locally)
- `arecord` and `aplay` (for recording test audio and hearing replies on Linux)

---

# USAGE

1. Load your environment variables (Check .env.example) (`cd server`) 

2. Start the server:
    ```bash
    set -a && source .env && set +a
    go run .
    ```

### TESTING WITHOUT HARDWARE
3. Record a 3-second .wav file:
    ```bash
    arecord -f S16_LE -r 16000 -c 1 -d 3 test_prompt.wav
    ```

4. In the second terminal run simulator for sending the file you recorded:
    ```bash
    go run ./cmd/mockmic
    ```
    You will see the in the first terminal, your request and your response.

5. Play the reponse audio:
    ```bash
    aplay -t raw -r 24 -f S16_LE -c 1 /tmp/reply.wav
    ```

---

# HOW IT WORKS

```text
Client -> UDP (1033B) -> Go sync.Pool + VAD -> STT -> Gemini -> TTS -> UDP (MsgTypeTTS)
```

### 1. Packet Format (9-Byte Header + Audio)
Every UDP packet starts with a 9-byte little-endian header followed by up to `512` signed 16-bit PCM samples (`1024` bytes = `32 ms` of audio at `16 kHz`):

| Bytes | Field | Type | Description |
| :--- | :--- | :--- | :--- |
| `0..3` | `Seq` | `uint32` | Packet sequence number |
| `4..5` | `Magic` | `uint16` | Fixed protocol ID (`0xAE05`) |
| `6..7` | `Length` | `uint16` | Payload byte length (`0..1024`) |
| `8` | `MsgType` | `uint8` | `1` = Mic audio, `2` = TTS audio, `3` = Stop |
| `9..1032` | `Payload` | `int16[]` | Raw PCM audio bytes |

Total max packet size is `1033` bytes, which stays under the `1472`-byte IPv4 UDP payload limit so packets don't get fragmented on Wi-Fi. In C (`firmware/include/protocol.h`), the struct uses `__attribute__((packed))` and `static_assert` so the compiler doesn't add padding bytes.

### 2. Server Design
- **Buffer reuse (`sync.Pool`):** Incoming packets are read into reused `1500`-byte `*[]byte` buffers instead of allocating a new slice per packet. `DecodePacket` slices the audio payload in place.
- **Non-blocking audio loop:** The main loop reads UDP packets, checks loudness (`ComputeRMS`), and collects speech into `speechBuf`. Once it detects ~700 ms of silence (`22` quiet packets), a `MsgTypeStop` packet, or `15` seconds of audio, it copies the utterance to a `2`-slot `utterances` channel using a non-blocking `select`. The UDP reader never waits on API calls.
- **Swappable AI providers:** `ai.Pipeline` talks to `STTProvider` and `TTSProvider` interfaces (`server/ai/provider.go`), so switching between all-Gemini, cloud Whisper/TTS, or local `whisper.cpp` + Piper only takes changing `.env`.
- **Paced reply streaming:** `aiWorker` sends TTS audio back to the sender's UDP address in `1024`-byte chunks spaced `20 ms` apart so a microcontroller's small socket buffer isn't overwhelmed.

---

# PROJECT STRUCTURE

```text
deskai/
├── .gitignore
├── README.md
├── firmware/
│   └── include/
└── server/
    ├── .env.example
    ├── go.mod
    ├── main.go               # UDP listener, sync.Pool, VAD, aiWorker, sendTTS
    ├── protocol/protocol.go  # EncodePacket / DecodePacket
    ├── audio/wav.go          # ComputeRMS and 44-byte WAV header builder
    ├── ai/
    │   ├── provider.go       # STTProvider / TTSProvider interfaces & env factory
    │   ├── pipeline.go       # STT -> Gemini -> TTS orchestration
    │   ├── gemini.go         # Gemini LLM client
    │   ├── stt_gemini.go     # Gemini STT
    │   ├── stt_whisper.go    # Whisper HTTP client (local whisper.cpp & Groq/OpenAI)
    │   ├── tts_gemini.go     # Gemini TTS
    │   ├── tts_cloud.go      # Cloud TTS HTTP client
    │   └── tts_piper.go      # Local Piper subprocess TTS
    └── cmd/mockmic/main.go   # Local WAV-over-UDP client simulator
```
