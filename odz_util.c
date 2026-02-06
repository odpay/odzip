#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include "odz.h"

void die(const char *m) { fprintf(stderr, "odz: error: %s\n", m); exit(1); }

void wr_u32le(uint8_t *dst, uint32_t x) {
	dst[0]=x&0xFF; dst[1]=(x>>8)&0xFF; dst[2]=(x>>16)&0xFF; dst[3]=(x>>24)&0xFF;
}

uint32_t rd_u32le(const uint8_t *src) {
	return (uint32_t)src[0] | ((uint32_t)src[1]<<8)
	     | ((uint32_t)src[2]<<16) | ((uint32_t)src[3]<<24);
}

void wr_u64le(uint8_t *dst, uint64_t x) {
	for (int i = 0; i < 8; i++) { dst[i] = (uint8_t)(x & 0xFF); x >>= 8; }
}

uint64_t rd_u64le(const uint8_t *src) {
	uint64_t x = 0;
	for (int i = 7; i >= 0; i--) x = (x << 8) | src[i];
	return x;
}
