package ai

import (
	"context"
	"errors"
	"log"
	"strings"

	"google.golang.org/genai"
)

type GeminiTTS struct {
	client *genai.Client
	model  string
	voice  string
}

func NewGeminiTTS(client *genai.Client, model, voice string) *GeminiTTS {
	if model == "" {
		model = "gemini-3.8-flash-tts"
	}
	if voice == "" {
		voice = "Kore"
	}
	return &GeminiTTS{client: client, model: model, voice: voice}
}

func (g *GeminiTTS) Synthesize(ctx context.Context, text string) ([]byte, error) {
	if strings.TrimSpace(text) == "" {
		return nil, errors.New("empty text")
	}
	log.Printf("GeminiTTS using model=%q voice=%q\n", g.model, g.voice)

	config := &genai.GenerateContentConfig{
		ResponseModalities: []string{"AUDIO"},
		SpeechConfig: &genai.SpeechConfig{
			VoiceConfig: &genai.VoiceConfig{
				PrebuiltVoiceConfig: &genai.PrebuiltVoiceConfig{
					VoiceName: g.voice,
				},
			},
		},
	}
	res, err := g.client.Models.GenerateContent(ctx, g.model, genai.Text(text), config)
	if err != nil {
		return nil, err
	}
	if len(res.Candidates) == 0 || res.Candidates[0].Content == nil {
		return nil, errors.New("no candidates returned by gemini")
	}

	for _, part := range res.Candidates[0].Content.Parts {
		if part.InlineData != nil && len(part.InlineData.Data) > 0 {
			log.Printf("Gemini TTS MIME: %s, bytes: %d\n", part.InlineData.MIMEType, len(part.InlineData.Data))
			data := part.InlineData.Data
			if len(data) > 44 && string(data[:4]) == "RIFF" {
				return data[44:], nil
			}
			return data, nil
		}

	}
	return nil, errors.New("No audio returned by gemini")

}
