typedef struct attribute((packed)) { uint32_t seq; uint16_t magic; uint16_t
length; uint8_t type; }PacketHeader;

static_assert(sizeof(PacketHeader) == 9, "Header size mismatch!");

typedef struct attribute((packed)){ PacketHeader header; int16_t samples[512];
}AudioPacket;
