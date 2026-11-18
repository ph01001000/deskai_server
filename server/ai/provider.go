package ai


import (
	"context"
)

type STTProvider interface{
	Transcribe(ctx context.Context, wavData []byte) (string, error)
}

type TTSProvider interface{
	Synthesize(ctx context.Context, text string) ([]byte, error)
}