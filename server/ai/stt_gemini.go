package ai

import (
	"context"
	"strings"

	"google.golang.org/genai"
)

type GeminiSTT struct {
	client *genai.Client
	model  string
}

func NewGeminiSTT(client *genai.Client, model string) *GeminiSTT {
	return &GeminiSTT{client: client, model: model}
}

func (g *GeminiSTT) Transcribe(ctx context.Context, wavData []byte) (string, error) {
	parts := []*genai.Part{
		{
			InlineData: &genai.Blob{
				MIMEType: "audio/wav",
				Data:     wavData,
			},
		},
		{
			Text: "Transcribe this audio verbatim. Output ONLY the plain transcription text, nothing else.",
		},
	}

	res, err := g.client.Models.GenerateContent(ctx, g.model, []*genai.Content{{Parts: parts}}, nil)
	if err != nil {
		return "", err
	}
	return strings.TrimSpace(res.Text()), nil
}
