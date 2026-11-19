package ai

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"mime/multipart"
	"net/http"
	"strings"
	"time"
)

type WhisperSTT struct {
	endpoint string
	apikey   string
	model    string
	client   *http.Client
}

func NewWhisperSTT(endpoint, apiKey, model string) *WhisperSTT {
	client := &http.Client{Timeout: 15 * time.Second}
	STT := WhisperSTT{endpoint: endpoint, apikey: apiKey, model: model, client: client}
	return &STT
}

func (w *WhisperSTT) Transcribe(ctx context.Context, wavData []byte) (string, error) {
	var body bytes.Buffer
	writer := multipart.NewWriter(&body)
	part, err := writer.CreateFormFile("file", "audio.wav")
	if err != nil {
		return "", err
	}
	_, err = part.Write(wavData)
	if err != nil {
		return "", err
	}
	writer.WriteField("response_format", "json")
	if w.model != "" {
		writer.WriteField("model", w.model)
	}
	err = writer.Close()
	if err != nil {
		return "", err
	}
	req, err := http.NewRequestWithContext(ctx, http.MethodPost, w.endpoint, &body)
	if err != nil {
		return "", err
	}

	req.Header.Set("Content-Type", writer.FormDataContentType())
	if w.apikey != "" {
		req.Header.Set("Authorization", "Bearer "+w.apikey)
	}
	resp, err := w.client.Do(req)
	if err != nil {
		return "", err
	}
	defer resp.Body.Close()

	if resp.StatusCode != http.StatusOK {
		errBody, _ := io.ReadAll(io.LimitReader(resp.Body, 4096))
		return "", fmt.Errorf("whisper error (%d): %s", resp.StatusCode, string(errBody))

	}

	var out struct {
		Text string `json:"text"`
	}

	err = json.NewDecoder(resp.Body).Decode(&out)
	if err != nil {
		return "", err
	}
	return strings.TrimSpace(out.Text), nil

}
