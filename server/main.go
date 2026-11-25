package main

import (
	"context"
	"encoding/binary"
	"fmt"
	"log"
	"net"
	"os"
	"os/exec"
	"path/filepath"
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

func aiWorker(aiBusy *atomic.Bool, conn *net.UDPConn, pipeline *ai.Pipeline, utterances <-chan utteranceJob, stopChan <-chan struct{}) {
	for job := range utterances {
		ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
		sendStatus(conn, job.addr, protocol.StateThinking, protocol.MoodNormal, "")
		mood, track, reply, synth, err := pipeline.ProcessUtterance(ctx, job.pcm)
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

		if mood == protocol.MoodCommand {
			executeSystemCommand(track)
		}
		if mood == protocol.MoodMusic {
			musicFile := filepath.Join("music", track)
			if track == "" {
				musicFile = "music/Metallica/01. Enter Sandman.flac"
			}
			select {
			case <-stopChan:
			default:
			}

			musicCtx, cancelMusic := context.WithCancel(context.Background())
			defer cancelMusic()

			go func() {
				select {
				case <-stopChan:
					log.Println("Interrupt triggered! Stopping music...")
					cancelMusic()
					aiBusy.Store(false)
				case <-musicCtx.Done():

				}
			}()

			if err := streamMusic(musicCtx, conn, job.addr, musicFile); err != nil {
				log.Printf("Music streaming ended: %v\n", err)
			}
			time.Sleep(300 * time.Millisecond)
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
	sendAudioConfig(conn, addr, 22050)
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

func streamMusic(ctx context.Context, conn *net.UDPConn, addr *net.UDPAddr, filePath string) error {

	sendAudioConfig(conn, addr, 24000)
	time.Sleep(10 * time.Millisecond)

	sendStatus(conn, addr, protocol.StateSpeaking, protocol.MoodCool, "♪ Playing Music ♪")

	pktBuf := make([]byte, protocol.HeaderSize+protocol.MaxSamples*2)
	var seq uint32 = 1

	return ai.StreamMP3(ctx, filePath, func(chunk []byte) error {
		n, err := protocol.EncodePacket(pktBuf, protocol.PacketHeader{Seq: seq, Msgtype: protocol.MsgTypeTTS}, chunk)
		if err != nil {
			return err
		}
		_, err = conn.WriteToUDP(pktBuf[:n], addr)
		if err != nil {
			return err
		}
		seq++
		return nil
	})
}

func executeSystemCommand(cmd string) {
	log.Printf("[CMD] Executing server action: %s\n", cmd)

	switch cmd {
	case "reboot":
		go func() {
			time.Sleep(4 * time.Second)
			// out, err := exec.Command("sudo", "reboot").CombinedOutput()
			// if err != nil {
			// log.Printf("[CMD] 'sudo reboot' failed: %v (output: %s)\n", err, string(out))
			out2, err2 := exec.Command("systemctl", "reboot").CombinedOutput()
			if err2 != nil {
				log.Printf("[CMD] 'systemctl reboot' also failed: %v (output: %s)\n", err2, string(out2))
			}
			// }
		}()

	case "suspend":
		go func() {
			time.Sleep(4 * time.Second)
			exec.Command("systemctl", "suspend").Run()
		}()

	case "docker_restart":
		go func() {
			exec.Command("sudo", "systemctl", "restart", "docker").Run()
		}()

	default:
		log.Printf("[CMD] Unknown or unauthorized command: %s\n", cmd)
	}
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
	stopChan := make(chan struct{}, 1)

	go aiWorker(&aiBusy, conn, pipeline, utterances, stopChan)

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

		if header.Msgtype == protocol.MsgTypeStop {
			select {
			case stopChan <- struct{}{}:
			default:
			}

			if recording && len(speechBuf) > 0 {
				log.Println("[PTT] Finalizing speech and dispatching to Gemini...")
				pcmCopy := append([]byte(nil), speechBuf...)
				select {
				case utterances <- utteranceJob{pcm: pcmCopy, addr: Addr}:
					aiBusy.Store(true)
					sendStatus(conn, Addr, protocol.StateThinking, protocol.MoodNormal, "")
				default:
					log.Print("AI busy, dropping utterance...")
				}
				speechBuf = speechBuf[:0]
				recording = false
				silenceCount = 0
				loudPackets = 0
				preRoll = preRoll[:0]
			}

			syncpool.Put(bufPtr)
			continue
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
