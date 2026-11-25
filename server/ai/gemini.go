package ai

import (
	"context"
	"errors"
	"fmt"
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

func (g *GeminiClient) Reply(ctx context.Context, prompt string, availableTracks []string, systemVitals string) (string, error) {
	if strings.TrimSpace(prompt) == "" {
		return "", errors.New("empty prompt")
	}
	trackListStr := strings.Join(availableTracks, ", ")

	systemPrompt := fmt.Sprintf(
		"You are a concise voice desk assistant. Reply in 1-3 plain spoken sentences with NO markdown, asterisks, or bullet points, in a cool badass manner. Start each reply with one of the tags: [NORMAL], [ANNOYED], [HAPPY], [LAUGH], [ANGRY], [SAD], [COOL], [MUSIC:<track>], [CMD:<action>]. Available music tracks: [%s]. Live Server Vitals: [%s]. If the user asks about the server, system status, temperature, RAM, CPU, or storage, use the live server vitals to answer accurately. If the user asks for music, pick a track with [MUSIC:<track>]. If the user asks to reboot or perform a server action, reply with [CMD:reboot] followed by a cool confirmation.",
		trackListStr,
		systemVitals,
	)

	res, err := g.client.Models.GenerateContent(ctx, g.model, genai.Text(prompt), &genai.GenerateContentConfig{
		SystemInstruction: &genai.Content{Parts: []*genai.Part{{Text: systemPrompt}}},
	})

	if err != nil {
		return "", err
	}
	return res.Text(), nil
}
