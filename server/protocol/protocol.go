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
