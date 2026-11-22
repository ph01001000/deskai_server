package ai

import (
	"context"
	"deskai/audio"
	"log"
)

type Pipeline struct {
	stt STTProvider
	llm *GeminiClient
	tts TTSProvider
}

func NewPipeline(stt STTProvider, llm *GeminiClient, tts TTSProvider) *Pipeline {
	return &Pipeline{stt: stt, llm: llm, tts: tts}
}

func (p *Pipeline) ProcessUtterance(ctx context.Context, pcm []byte) (string, []byte, error) {
	if len(pcm) == 0 {
		return "", nil, nil
	}
	raw := audio.WrapWAV(pcm)
	text, err := p.stt.Transcribe(ctx, raw)
	if err != nil {
		return "", nil, err
	}
	log.Printf("User: %s\n", text)

	reply, err := p.llm.Reply(ctx, text)
	if err != nil {
		return "", nil, err
	}
	log.Printf("AI: %s\n", reply)

	synth, err := p.tts.Synthesize(ctx, reply)
	if err != nil {
		return "", nil, err
	}
	return reply, synth, nil
}
