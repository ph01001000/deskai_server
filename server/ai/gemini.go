package ai

import (
	"context"
	"errors"
	"strings"

	"google.golang.org/genai"
)

type GeminiClient struct {
	client *genai.Client
	model  string
}

func NewGeminiClient(ctx context.Context, apiKey string) (*GeminiClient, error) {
	if len(apiKey) == 0 {
		return nil, errors.New("empty API key")
	}
	nclient, err := genai.NewClient(ctx, &genai.ClientConfig{
		APIKey:  apiKey,
		Backend: genai.BackendGeminiAPI,
		HTTPOptions: genai.HTTPOptions{
			APIVersion: "v1beta",
		},
	})
	if err != nil {
		return nil, err
	}
	newClient := GeminiClient{
		client: nclient,
		model:  "gemini-3.5-flash-lite", // stable, fast, listed in your out.txt!
	}
	return &newClient, nil
}

func (g *GeminiClient) Reply(ctx context.Context, prompt string) (string, error) {
	if strings.TrimSpace(prompt) == "" {
		return "", errors.New("empty prompt")
	}

	res, err := g.client.Models.GenerateContent(ctx, g.model, genai.Text(prompt), &genai.GenerateContentConfig{
		SystemInstruction: &genai.Content{Parts: []*genai.Part{{Text: "You are a concise voice desk assistant. Reply in 1-3 plain spoken sentences with NO markdown, asterisks, or bullet points, talk and answer question in a cool badass manner, an assistant and a cool friend and badass does not mean genz, be cool, and start each reply with one of the following tags: [NORMAL], [ANNOYED], [HAPPY], [LAUGH], [ANGRY], [SAD], [COOL]. for example : [COOL] Relax, I already checked the logs."}}},
	})

	if err != nil {
		return "", err
	}
	return res.Text(), nil
}
