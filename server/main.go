package main

import (
	"context"
	"encoding/binary"
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
	"sync/atomic"
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

func aiWorker(aiBusy *atomic.Bool, conn *net.UDPConn, pipeline *ai.Pipeline, utterances <-chan utteranceJob) {
	for job := range utterances {
		ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
		sendStatus(conn, job.addr, protocol.StateThinking, protocol.MoodNormal, "")
		mood, reply, synth, err := pipeline.ProcessUtterance(ctx, job.pcm)
		cancel()

		if err != nil {
			log.Print(err)
			sendStatus(conn, job.addr, protocol.StateIdle, protocol.MoodNormal, "")
			aiBusy.Store(false)
			continue
		}
		if len(reply) > 0 && len(synth) > 0 {
			err := os.WriteFile("/tmp/reply.wav", audio.WrapWAV(synth), 0644)
			if err != nil {
				log.Print(err)
			}
			sendStatus(conn, job.addr, protocol.StateSpeaking, mood, reply)
			sendTTS(conn, job.addr, synth)
			time.Sleep(800 * time.Millisecond)
		}
		sendStatus(conn, job.addr, protocol.StateIdle, protocol.MoodNormal, "")
		aiBusy.Store(false)
	}
}

func sendStatus(conn *net.UDPConn, addr *net.UDPAddr, state, mood uint8, text string) {
	if len(text) > 1022 {
		text = text[:1022]
	}
	payload := make([]byte, 2+len(text))
	payload[0] = state
	payload[1] = mood

	copy(payload[2:], text)
	pktBuf := make([]byte, protocol.HeaderSize+len(payload))
	n, err := protocol.EncodePacket(pktBuf, protocol.PacketHeader{Seq: 0, Msgtype: protocol.MsgTypeStatus}, payload)
	if err != nil {
		return
	}
	conn.WriteToUDP(pktBuf[:n], addr)
}

func sendTTS(conn *net.UDPConn, addr *net.UDPAddr, pcm []byte) {
	sendAudioConfig(conn, addr, 24000)
	time.Sleep(5 * time.Millisecond)
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

func sendAudioConfig(conn *net.UDPConn, addr *net.UDPAddr, sampleRate uint32) {
	payload := make([]byte, 4)
	binary.LittleEndian.PutUint32(payload, sampleRate)
	pktBuf := make([]byte, protocol.HeaderSize+len(payload))
	n, err := protocol.EncodePacket(pktBuf, protocol.PacketHeader{Seq: 0, Msgtype: protocol.MsgTypeAudioConfig}, payload)
	if err != nil {
		return
	}
	conn.WriteToUDP(pktBuf[:n], addr)
}

func main() {
	var aiBusy atomic.Bool

	if err := ensureEnv(); err != nil {
		log.Fatalf("Setup failed: %v", err)
	}

	api := os.Getenv("GEMINI_API_KEY")
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
	utterances := make(chan utteranceJob, 1)

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

	go aiWorker(&aiBusy, conn, pipeline, utterances)

	var speechBuf []byte
	recording := false
	silenceCount := 0
	loudPackets := 0
	const maxPreRoll = 4 * 1024
	var preRoll []byte

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
			// fmt.Printf("%s, %d, %d ,%d \n", Addr, header.Seq, header.Msgtype, header.Length) // Uncomment for debug to check if packets are being received..
		}

		if aiBusy.Load() {
			syncpool.Put(bufPtr)
			speechBuf = speechBuf[:0]
			recording = false
			silenceCount = 0
			loudPackets = 0
			preRoll = preRoll[:0]
			continue
		}

		if header.Msgtype == protocol.MsgTypeMic {
			rms := audio.ComputeRMS(payload)
			if header.Seq%31 == 0 {
				fmt.Printf("RX from %s seq=%d rms=%.1f\n", Addr, header.Seq, rms)
			}
			if len(preRoll) > maxPreRoll {
				preRoll = preRoll[len(preRoll)-maxPreRoll:]
			}
			if rms > 1500 { // Change this value according to your room average rms peak, check first by testing mic.
				if !recording {
					speechBuf = append(speechBuf, preRoll...)
					preRoll = preRoll[:0]
					recording = true
				}
				loudPackets++
				silenceCount = 0
				if loudPackets == 2 {
					sendStatus(conn, Addr, protocol.StateListening, protocol.MoodNormal, "")
				}
			} else if recording {
				silenceCount++
			}
			if recording {
				speechBuf = append(speechBuf, payload...)
			} else {
				preRoll = append(preRoll, payload...)
			}

		}

		syncpool.Put(bufPtr)

		if (recording && silenceCount >= 45 && len(speechBuf) >= 16000 && loudPackets >= 4) || (header.Msgtype == protocol.MsgTypeStop && len(speechBuf) > 0) || len(speechBuf) > 16000*2*15 {
			pcmCopy := append([]byte(nil), speechBuf...)
			select {
			case utterances <- utteranceJob{pcm: pcmCopy, addr: Addr}:
				aiBusy.Store(true)
			default:
				log.Print("AI busy, dropping utterances...")
			}

			speechBuf = speechBuf[:0]
			recording = false
			silenceCount = 0
			loudPackets = 0
		} else if recording && silenceCount >= 45 {
			if loudPackets >= 2 {
				sendStatus(conn, Addr, protocol.StateIdle, protocol.MoodNormal, "")
			}
			loudPackets = 0
			silenceCount = 0
			recording = false
			speechBuf = speechBuf[:0]
			preRoll = preRoll[:0]
		}
	}
}
