package main

import (
	"context"
	"fmt"
	"log"
	"net"
	"os"
	"sync"
	"time"

	// "error"
	"deskai/ai"
	"deskai/audio"
	"deskai/protocol"
)

var syncpool = sync.Pool{
	New: func() any {
		b := make([]byte, 1500)
		return &b
	},
}

type utteranceJob struct {
	pcm  []byte
	addr *net.UDPAddr
}

func aiWorker(conn *net.UDPConn, pipeline *ai.Pipeline, utterances <-chan utteranceJob) {
	for job := range utterances {
		ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
		reply, err := pipeline.ProcessUtterance(ctx, job.pcm)
		cancel()
		if err != nil {
			log.Print(err)
			continue
		}
		if len(reply) > 0 {
			err := os.WriteFile("/tmp/reply.wav", audio.WrapWAV(reply), 0644)
			if err != nil {
				log.Print(err)
			}
			sendTTS(conn, job.addr, reply)
		}

	}
}

func sendTTS(conn *net.UDPConn, addr *net.UDPAddr, pcm []byte) {
	pktBuf := make([]byte, protocol.HeaderSize+protocol.MaxSamples*2)
	var seq uint32 = 1
	for i := 0; i < len(pcm); i += protocol.MaxSamples * 2 {
		end := min(i+protocol.MaxSamples*2, len(pcm))
		n, err := protocol.EncodePacket(pktBuf, protocol.PacketHeader{Seq: seq, Msgtype: protocol.MsgTypeTTS}, pcm[i:end])
		if err != nil {
			log.Print("Packet couldn't be encoded")
			return
		}

		_, err = conn.WriteToUDP(pktBuf[:n], addr)
		if err != nil {
			log.Print("Couldn't send packet")
			return
		}
		seq++
		time.Sleep(15 * time.Millisecond)
	}

}

func main() {
	api := os.Getenv("GEMINI_API_KEY")
	if api == "" {
		log.Fatal("Empty API key")
	}
	addr := &net.UDPAddr{
		Port: 8000,
		IP:   net.ParseIP("0.0.0.0"),
	}

	conn, err := net.ListenUDP("udp", addr)
	if err != nil {
		log.Fatalf("listener not started: %v", err)
	}
	defer conn.Close()

	fmt.Printf("UDP server listening on 8000: \n")
	utterances := make(chan utteranceJob, 2)

	llm, err := ai.NewGeminiClient(context.Background(), api)
	if err != nil {
		log.Fatalf("Unable to create llm: %v", err)
	}
	stt, err := ai.NewSTTFromEnv()
	if err != nil {
		log.Fatal(err)
	}
	tts, err := ai.NewTTSFromEnv()
	if err != nil {
		log.Fatal(err)
	}
	pipeline := ai.NewPipeline(stt, llm, tts)

	go aiWorker(conn, pipeline, utterances)

	var speechBuf []byte
	recording := false
	silenceCount := 0

	for {
		bufPtr := syncpool.Get().(*[]byte)
		n, Addr, err := conn.ReadFromUDP(*bufPtr)
		if err != nil {
			fmt.Println(err)
			syncpool.Put(bufPtr)
			continue
		}

		header, payload, err := protocol.DecodePacket((*bufPtr)[:n])
		if err != nil {
			syncpool.Put(bufPtr)
			continue
		} else {
			fmt.Printf("%s, %d, %d ,%d \n", Addr, header.Seq, header.Msgtype, header.Length)
		}

		if header.Msgtype == protocol.MsgTypeMic {
			rms := audio.ComputeRMS(payload)
			if header.Seq%31 == 0 {
				fmt.Printf("RX from %s seq=%d rms=%.1f\n", Addr, header.Seq, rms)
			}
			if rms > 2000 { // Change this line according to your room average rms peak, check first by testing mic.
				recording = true
				silenceCount = 0
			} else if recording {
				silenceCount++
			}
			if recording {
				speechBuf = append(speechBuf, payload...)
			}

		}

		syncpool.Put(bufPtr)

		if (recording && silenceCount >= 22) || (header.Msgtype == protocol.MsgTypeStop && len(speechBuf) > 0) || len(speechBuf) > 16000*2*15 {
			pcmCopy := append([]byte(nil), speechBuf...)
			select {
			case utterances <- utteranceJob{pcm: pcmCopy, addr: Addr}:

			default:
				log.Print("AI busy, dropping utterances...")
			}

			speechBuf = speechBuf[:0]
			recording = false
			silenceCount = 0
		}
	}
}
