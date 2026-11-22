#pragma once
#include <stdint.h>

typedef struct __attribute__((packed)) {
    uint32_t seq;
    uint16_t magic;
    uint16_t length;
    uint8_t  type;
} PacketHeader;

static_assert(sizeof(PacketHeader) == 9, "Header size mismatch!");

typedef struct __attribute__((packed)) {
    PacketHeader header;
    int16_t samples[512];
} AudioPacket;

static_assert(sizeof(AudioPacket) == 1033, "Header size mismatch for audio");