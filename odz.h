#ifndef ODZ_H
#define ODZ_H
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

/* ── Format constants ──────────────────────────────────────── */
#define ODZ_VERSION     2
#define ODZ_WINDOW      32768u      /* max back-reference distance */
#define ODZ_MIN_MATCH   3
#define ODZ_MAX_MATCH   258
#define ODZ_BLOCK_SIZE  (1u << 20)  /* 1 MB blocks for streaming */

/* Block types (bits 1-2 of block_flags) */
#define ODZ_BLOCK_STORED    0
#define ODZ_BLOCK_HUFFMAN   1

/* ── Utilities ─────────────────────────────────────────────── */
void     die(const char *m);
void     wr_u32le(uint8_t *dst, uint32_t x);
uint32_t rd_u32le(const uint8_t *src);
void     wr_u64le(uint8_t *dst, uint64_t x);
uint64_t rd_u64le(const uint8_t *src);

/* ── Streaming API ─────────────────────────────────────────── */
void odz_compress(FILE *in, FILE *out);
void odz_decompress(FILE *in, FILE *out);

#endif
