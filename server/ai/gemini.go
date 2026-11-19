package ai

import (
	"context"
	"errors"
	"google.golang.org/genai"
	"strings"
)

type GeminiClient struct {
	client *genai.Client
	model  string
}

func NewGeminiClient(ctx context.Context, apiKey string) (*GeminiClient, error) {
	if len(apiKey) == 0 {
		return nil, errors.New("Empty Api")
	}
	nclient, err := genai.NewClient(ctx, &genai.ClientConfig{APIKey: apiKey, Backend: genai.BackendGeminiAPI})
	if err != nil {
		return nil, err
	}
	NewClient := GeminiClient{client: nclient, model: "gemini-flash-latest"}
	return &NewClient, nil
}

func (g *GeminiClient) Reply(ctx context.Context, prompt string) (string, error) {
	if strings.TrimSpace(prompt) == "" {
		return "", errors.New("empty prompt")
	}

	res, err := g.client.Models.GenerateContent(ctx, g.model, genai.Text(prompt), &genai.GenerateContentConfig{
		SystemInstruction: &genai.Content{Parts: []*genai.Part{{Text: "You are a concise voice desk assistant. Reply in 1-3 plain spoken sentences with NO markdown, asterisks, or bullet points, talk and answer question in a cool badass manner, an assistant and a cool friend."}}},
	})

	if err != nil {
		return "", err
	}
	return res.Text(), nil
}
