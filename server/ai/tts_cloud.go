package ai

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"strings"
)

type CloudTTS struct {
	endpoint string
	apikey   string
	model    string
	voice    string
	client   *http.Client
}

func NewCloudTTS(endpoint, apikey, model, voice string) *CloudTTS {
	return &CloudTTS{endpoint: endpoint, apikey: apikey, model: model, voice: voice}
}

func (c *CloudTTS) Synthesize(ctx context.Context, text string) ([]byte, error) {
	if strings.TrimSpace(text) == "" {
		return nil, errors.New("empty text")
	}
	requestpayload := struct {
		Model          string `json:"model"`
		Input          string `json:"input"`
		Voice          string `json:"voice"`
		ResponseFormat string `json:"response_format"`
		SampleRate     int    `json:"sample_rate,omitempty"`
	}{
		Model:          c.model,
		Input:          text,
		Voice:          c.voice,
		ResponseFormat: "wav",
		SampleRate:     16000,
	}

	jsonbytes, err := json.Marshal(requestpayload)
	if err != nil {
		return nil, err
	}

	req, err := http.NewRequestWithContext(ctx, http.MethodPost, c.endpoint, bytes.NewReader(jsonbytes))
	if err != nil {
		return nil, err
	}
	req.Header.Set("Content-Type", "application/json")
	req.Header.Set("Authorization", "Bearer "+c.apikey)

	resp, err := c.client.Do(req)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()

	raw, err := io.ReadAll(resp.Body)
	if err != nil {
		return nil, err
	}
	if resp.StatusCode != http.StatusOK {
		return nil, fmt.Errorf("cloud tts (%d): %s", resp.StatusCode, string(raw))
	}
	if len(raw) > 44 && string(raw[:4]) == "RIFF" {
		return raw[44:], nil
	}
	return raw, nil
}
