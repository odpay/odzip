#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include "odz.h"

void die(const char *m) { fprintf(stderr, "err: %s\n", m); exit(1); }

void wr_u32le(uint8_t *dst, uint32_t x) {
	dst[0]=x&0xFF; dst[1]=(x>>8)&0xFF; dst[2]=(x>>16)&0xFF; dst[3]=(x>>24)&0xFF;
}
uint32_t rd_u32le(const uint8_t *src) {
	return (uint32_t)src[0] | ((uint32_t)src[1]<<8) | ((uint32_t)src[2]<<16) | ((uint32_t)src[3]<<24);
}
