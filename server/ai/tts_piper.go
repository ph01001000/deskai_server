package ai

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"os/exec"
	"strings"
)

type PiperTTS struct {
	binaryPath string
	modelPath  string
}

func NewPiperTTS(binaryPath, modelPath string) *PiperTTS {
	return &PiperTTS{binaryPath: binaryPath, modelPath: modelPath}
}
func (p *PiperTTS) Synthesize(ctx context.Context, text string) ([]byte, error) {
	if strings.TrimSpace(text) == "" {
		return nil, errors.New("empty text")
	}
	cmd := exec.CommandContext(ctx, p.binaryPath, "--model", p.modelPath, "--output-raw")

	cmd.Stdin = strings.NewReader(text)

	var out, errBuf bytes.Buffer
	cmd.Stdout = &out
	cmd.Stderr = &errBuf
	if err := cmd.Run(); err != nil {
		return nil, fmt.Errorf("piper failed (%w): %s", err, errBuf.String())
	}
	return out.Bytes(), nil
}
