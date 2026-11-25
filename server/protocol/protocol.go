package protocol

import (
	"encoding/binary"
	"errors"
)

const MagicValue = 0xAE05
const HeaderSize = 9
const MaxSamples = 512
const MsgTypeMic = 1
const MsgTypeTTS = 2
const MsgTypeStop = 3
const MsgTypeStatus = 4
const MsgTypeAudioConfig = 5
const StateListening = 0
const StateThinking = 1
const StateSpeaking = 2
const StateIdle = 3

const MoodNormal = 0
const MoodAnnoyed = 1
const MoodHappy = 2
const MoodLaugh = 3
const MoodAngry = 4
const MoodSad = 5
const MoodCool = 6
const MoodMusic = 7
const MoodCommand = 8

type PacketHeader struct {
	Seq     uint32
	Magic   uint16
	Length  uint16
	Msgtype uint8
}

func DecodePacket(buf []byte) (PacketHeader, []byte, error) {
	if len(buf) < HeaderSize {
		return PacketHeader{}, nil, errors.New("header size smaller")
	}

	var hdr PacketHeader
	hdr.Seq = binary.LittleEndian.Uint32(buf[0:4])
	hdr.Magic = binary.LittleEndian.Uint16(buf[4:6])
	hdr.Length = binary.LittleEndian.Uint16(buf[6:8])
	hdr.Msgtype = buf[8]

	if hdr.Magic != MagicValue {
		return PacketHeader{}, nil, errors.New("Magic value not matched")
	}

	if int(hdr.Length) > MaxSamples*2 {
		return PacketHeader{}, nil, errors.New("Length bigger than Maxsamplesx2")
	}

	if len(buf) < HeaderSize+int(hdr.Length) {
		return PacketHeader{}, nil, errors.New("truncated payload")
	}

	return hdr, buf[HeaderSize : HeaderSize+int(hdr.Length)], nil
}

func EncodePacket(buf []byte, hdr PacketHeader, payload []byte) (int, error) {
	if len(payload) > MaxSamples*2 {
		return 0, errors.New("payload size bigger than SampleSize")
	}
	if len(buf) < HeaderSize+len(payload) {
		return 0, errors.New("dst buffer too small")
	}

	binary.LittleEndian.PutUint32(buf[0:4], hdr.Seq)
	binary.LittleEndian.PutUint16(buf[4:6], MagicValue)
	binary.LittleEndian.PutUint16(buf[6:8], uint16(len(payload)))
	buf[8] = hdr.Msgtype

	copy(buf[HeaderSize:], payload)

	return HeaderSize + len(payload), nil

}
