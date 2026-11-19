package main

import (
	"deskai/protocol"
	"log"
	"net"
	"os"
	"time"
)

func main() {
	conn, err := net.DialUDP("udp", nil, &net.UDPAddr{IP: net.ParseIP("127.0.0.1"), Port: 8000})
	if err != nil {
		log.Fatalf("cannot reach: %v", err)
	}
	defer conn.Close()

	buf := make([]byte, 1033)
	raw, err := os.ReadFile("test_prompt.wav")
	if err != nil {
		log.Fatal(err)
	}
	if len(raw) <= 44 {
		log.Fatal("wav too short")
	}
	pcm := raw[44:]
	var seq uint32 = 1
	for i := 0; i < len(pcm); i += 1024 {
		end := min(i+1024, len(pcm))
		packetsize, err := protocol.EncodePacket(buf, protocol.PacketHeader{Seq: seq, Msgtype: protocol.MsgTypeMic}, pcm[i:end])
		if err != nil {
			log.Fatal(err)
		}
		if _, err := conn.Write(buf[:packetsize]); err != nil {
			log.Fatal(err)
		}
		time.Sleep(32 * time.Millisecond)
		seq++
	}
	last, err := protocol.EncodePacket(buf, protocol.PacketHeader{Seq: seq, Msgtype: protocol.MsgTypeStop}, nil)
	if err != nil {
		log.Fatal(err)
	}
	if _, err := conn.Write(buf[:last]); err != nil {
		log.Fatal(err)
	}
}
