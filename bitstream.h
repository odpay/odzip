#ifndef BITSTREAM_H
#define BITSTREAM_H

#include <stdint.h>
#include <stddef.h>

/* ── Memory-backed bit writer ──────────────────────────────── */
typedef struct {
    uint8_t *buf;
    size_t   cap;
    size_t   pos;       /* next byte position */
    uint64_t bits;      /* accumulator */
    int      nbits;     /* valid bits in accumulator */
} bit_writer_t;

void bw_init(bit_writer_t *w, size_t initial_cap);
void bw_free(bit_writer_t *w);
void bw_write(bit_writer_t *w, uint32_t val, int nbits);   /* LSB-first */
void bw_flush(bit_writer_t *w);                             /* pad to byte boundary */

/* ── Memory-backed bit reader ──────────────────────────────── */
typedef struct {
    const uint8_t *buf;
    size_t         len;
    size_t         pos;     /* next byte position */
    uint64_t       bits;    /* accumulator */
    int            nbits;   /* valid bits in accumulator */
} bit_reader_t;

void     br_init(bit_reader_t *r, const uint8_t *buf, size_t len);
uint32_t br_peek(bit_reader_t *r, int nbits);
uint32_t br_read(bit_reader_t *r, int nbits);   /* LSB-first */

/* Lightweight consume after br_peek — just shifts bits, no refill */
static inline void br_consume(bit_reader_t *r, int nbits) {
    r->bits >>= nbits;
    r->nbits -= nbits;
}

#endif
