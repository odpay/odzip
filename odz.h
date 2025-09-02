#ifndef ODZ_H
#define ODZ_H
#include <stdint.h>

#define ODZ_VERSION   1
#define ODZ_WINDOW    65535u
#define ODZ_MIN_MATCH 3
#define ODZ_MAX_MATCH 258

void die(const char *m);
void wr_u32le(uint8_t *dst, uint32_t x);
uint32_t rd_u32le(const uint8_t *src);

void compress_simple(const uint8_t *in, size_t n, uint8_t **outp, size_t *outn);
void decompress_simple(const uint8_t *in, size_t n, uint8_t **outp, size_t *outn);


#endif