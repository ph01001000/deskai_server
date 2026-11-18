package audio

import (
	"encoding/binary"
	"math"
)

func ComputeRMS(pcm []byte) float64 {
	if len(pcm) < 2 {
		return 0
	}
	var sum float64
	for i := 0; i+1 < len(pcm); i += 2 {
		s := int16(binary.LittleEndian.Uint16(pcm[i : i+2]))
		f := float64(s)
		sum = sum + (f * f)
	}
	mean := sum / float64(len(pcm)/2)
	return math.Sqrt(mean)
}

func WrapWAV(pcm []byte) []byte {
	out := make([]byte, 44+len(pcm))
	copy(out[0:4], "RIFF")
	binary.LittleEndian.PutUint32(out[4:8], uint32(36+len(pcm)))

	copy(out[8:12], "WAVE")
	copy(out[12:16], "fmt ")
	// copy(out[16:20],uint32(16))
	binary.LittleEndian.PutUint32(out[16:20], uint32(16))
	// copy(out[20:22],uint16(1))
	// copy(out[22:24],uint16(1))
	binary.LittleEndian.PutUint16(out[20:22], uint16(1))
	binary.LittleEndian.PutUint16(out[22:24], uint16(1))

	// copy(out[24:28],uint32(16000))
	binary.LittleEndian.PutUint32(out[24:28], uint32(16000))

	// copy(out[28:32],uint32(32000))
	binary.LittleEndian.PutUint32(out[28:32], uint32(32000))

	// copy(out[32:34],uint16(2))
	binary.LittleEndian.PutUint16(out[32:34], uint16(2))

	// copy(out[34:36],uint16(16))
	binary.LittleEndian.PutUint16(out[34:36], uint16(16))

	copy(out[36:40], "data")
	// copy(out[40:44], len(pcm))
	binary.LittleEndian.PutUint32(out[40:44], uint32(len(pcm)))

	copy(out[44:], pcm)

	return out
}
