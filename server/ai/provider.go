package ai

import (
	"context"
	"errors"
	"fmt"
	"os"
	"strings"

	"google.golang.org/genai"
)

// const endpoint="https://api.groq.com/openai/v1/audio/transcriptions"

type STTProvider interface {
	Transcribe(ctx context.Context, wavData []byte) (string, error)
}

type TTSProvider interface {
	Synthesize(ctx context.Context, text string) ([]byte, error)
}

func getEnvOrDefault(key, defaultValue string) string {
	val := strings.TrimSpace(os.Getenv(key))
	if val == "" {
		return defaultValue
	}
	return val
}

func NewSTTFromEnv() (STTProvider, error) {
	provider := strings.ToLower(getEnvOrDefault("STT_PROVIDER", "local"))
	switch provider {
	case "gemini":
		apikey := strings.TrimSpace(os.Getenv("GEMINI_API_KEY"))
		if apikey == "" {
			return nil, errors.New("GEMINI_API_KEY required for gemini STT")
		}
		client, err := genai.NewClient(context.Background(), &genai.ClientConfig{
			APIKey:      apikey,
			Backend:     genai.BackendGeminiAPI,
			HTTPOptions: genai.HTTPOptions{APIVersion: "v1beta"},
		})
		if err != nil {
			return nil, err
		}
		model := getEnvOrDefault("STT_MODEL", "gemini-2.5-flash")
		return NewGeminiSTT(client, model), nil
	case "cloud":
		api := strings.TrimSpace(os.Getenv("STT_API_KEY"))
		if api == "" {
			return nil, errors.New("API key empty for STT")
		}
		endpoint := strings.TrimSpace(getEnvOrDefault("STT_ENDPOINT", "https://api.groq.com/openai/v1/audio/transcriptions"))
		model := strings.TrimSpace(getEnvOrDefault("STT_MODEL", "whisper-large-v3-turbo"))
		return NewWhisperSTT(endpoint, api, model), nil
	case "local":
		endpoint := "http://127.0.0.1:8080/inference"
		return NewWhisperSTT(endpoint, "", ""), nil
	default:
		return nil, fmt.Errorf("unknown STT_PROVIDER: %s", provider)
	}
}

func NewTTSFromEnv() (TTSProvider, error) {
	provider := getEnvOrDefault("TTS_PROVIDER", "local")
	switch provider {
	case "gemini":
		key := strings.TrimSpace(os.Getenv("GEMINI_API_KEY"))
		if key == "" {
			return nil, errors.New("GEMINI_API_KEY required for gemini TTS")
		}
		client, err := genai.NewClient(context.Background(), &genai.ClientConfig{
			APIKey:      key,
			Backend:     genai.BackendGeminiAPI,
			HTTPOptions: genai.HTTPOptions{APIVersion: "v1beta"},
		})
		if err != nil {
			return nil, err
		}
		model := getEnvOrDefault("TTS_MODEL", "gemini-3.8-flash-lite-tts")
		voice := getEnvOrDefault("TTS_VOICE", "Kore")
		return NewGeminiTTS(client, model, voice), nil
	case "cloud":
		api := strings.TrimSpace(os.Getenv("TTS_API_KEY"))
		if api == "" {
			return nil, errors.New("API key empty for TTS")
		}
		endpoint := strings.TrimSpace(os.Getenv("TTS_ENDPOINT"))
		if endpoint == "" {
			return nil, errors.New("Empty endpoint TTS")
		}
		model := getEnvOrDefault("TTS_MODEL", "whisper-large-v3-turbo")
		voice := strings.TrimSpace(os.Getenv("TTS_VOICE"))
		return NewCloudTTS(endpoint, api, model, voice), nil
	case "local":
		bin := getEnvOrDefault("PIPER_BIN", "piper")
		model := strings.TrimSpace(os.Getenv("PIPER_MODEL"))
		if model == "" {
			return nil, errors.New("Empty model for TTS")
		}
		return NewPiperTTS(bin, model), nil
	default:
		return nil, fmt.Errorf("unknown TTS_PROVIDER: %s", provider)

	}
}
