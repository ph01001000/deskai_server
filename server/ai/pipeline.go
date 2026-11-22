package ai

import (
	"context"
	"deskai/audio"
	"deskai/protocol"
	"log"
	"strings"
)

type Pipeline struct {
	stt STTProvider
	llm *GeminiClient
	tts TTSProvider
}

func NewPipeline(stt STTProvider, llm *GeminiClient, tts TTSProvider) *Pipeline {
	return &Pipeline{stt: stt, llm: llm, tts: tts}
}

func (p *Pipeline) ProcessUtterance(ctx context.Context, pcm []byte) (uint8, string, []byte, error) {
	if len(pcm) == 0 {
		return 0, "", nil, nil
	}
	raw := audio.WrapWAV(pcm)
	text, err := p.stt.Transcribe(ctx, raw)
	if err != nil {
		return 0, "", nil, err
	}
	cleanIn := strings.TrimSpace(text)
	if strings.HasPrefix(cleanIn, "[") || strings.HasPrefix(cleanIn, "(") {
		return 0, "", nil, nil
	}
	if cleanIn == "" || cleanIn == "0:00" || cleanIn == "00:00" {
		return 0, "", nil, nil
	}
	log.Printf("User: %s\n", cleanIn)

	reply, err := p.llm.Reply(ctx, cleanIn)
	if err != nil {
		return 0, "", nil, err
	}
	log.Printf("AI: %s\n", reply)
	mood, cleanReply := parseMood(reply)
	synth, err := p.tts.Synthesize(ctx, cleanReply)
	if err != nil {
		return 0, "", nil, err
	}
	return mood, cleanReply, synth, nil
}

func parseMood(raw string) (uint8, string) {
	s := strings.TrimSpace(raw)
	var mood uint8
	if strings.HasPrefix(s, "[") {
		end := strings.Index(s, "]")
		if end > 0 && end < 15 {
			tag := strings.ToUpper(s[:end+1])
			clean := strings.TrimSpace(s[end+1:])
			switch tag {
			case "[ANNOYED]":
				mood = protocol.MoodAnnoyed
			case "[HAPPY]":
				mood = protocol.MoodHappy
			case "[LAUGH]":
				mood = protocol.MoodLaugh
			case "[ANGRY]":
				mood = protocol.MoodAngry
			case "[SAD]":
				mood = protocol.MoodSad
			case "[COOL]":
				mood = protocol.MoodCool
			default:
				mood = protocol.MoodNormal
			}
			return mood, clean
		}

	}
	return protocol.MoodNormal, s
}
