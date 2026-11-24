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

func (p *Pipeline) ProcessUtterance(ctx context.Context, pcm []byte) (uint8, string, string, []byte, error) {
	if len(pcm) == 0 {
		return 0, "", "", nil, nil
	}
	raw := audio.WrapWAV(pcm)

	text, err := p.stt.Transcribe(ctx, raw)
	if err != nil {
		return 0, "", "", nil, err
	}
	cleanIn := strings.TrimSpace(text)
	if strings.HasPrefix(cleanIn, "[") || strings.HasPrefix(cleanIn, "(") {
		return 0, "", "", nil, nil
	}
	if cleanIn == "" || cleanIn == "0:00" || cleanIn == "00:00" {
		return 0, "", "", nil, nil
	}
	log.Printf("User: %s\n", cleanIn)
	tracks := GetAvailableTracks("music")
	reply, err := p.llm.Reply(ctx, cleanIn, tracks)
	if err != nil {
		return 0, "", "", nil, err
	}
	log.Printf("AI: %s\n", reply)
	mood, music, cleanReply := parseMood(reply)
	synth, err := p.tts.Synthesize(ctx, cleanReply)
	if err != nil {
		return 0, "", "", nil, err
	}
	return mood, music, cleanReply, synth, nil
}

func parseMood(raw string) (uint8, string, string) {
	s := strings.TrimSpace(raw)
	var mood uint8 = protocol.MoodNormal
	var track string

	if strings.HasPrefix(s, "[") {
		end := strings.Index(s, "]")
		if end > 0 && end < 120 {
			tag := s[:end+1]
			clean := strings.TrimSpace(s[end+1:])
			tagUpper := strings.ToUpper(tag)

			if strings.HasPrefix(tagUpper, "[MUSIC") {
				mood = protocol.MoodMusic
				if strings.HasPrefix(tagUpper, "[MUSIC:") {
					track = strings.TrimSpace(tag[7 : len(tag)-1])
				}
				return mood, track, clean
			}

			switch tagUpper {
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
			return mood, track, clean
		}
	}
	return protocol.MoodNormal, "", s
}
