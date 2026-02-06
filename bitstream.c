#include "bitstream.h"
#include "odz.h"
#include <stdlib.h>
#include <string.h>

/* ── Writer ────────────────────────────────────────────────── */

void bw_init(bit_writer_t *w, size_t initial_cap) {
    w->buf = malloc(initial_cap);
    if (!w->buf) die("oom");
    w->cap = initial_cap;
    w->pos = 0;
    w->bits = 0;
    w->nbits = 0;
}

void bw_free(bit_writer_t *w) {
    free(w->buf);
    w->buf = NULL;
    w->cap = w->pos = 0;
}

static void bw_grow(bit_writer_t *w, size_t need) {
    while (w->pos + need >= w->cap) {
        w->cap = w->cap * 2 + 1024;
        w->buf = realloc(w->buf, w->cap);
        if (!w->buf) die("oom");
    }
}

void bw_write(bit_writer_t *w, uint32_t val, int nbits) {
    w->bits |= (uint64_t)val << w->nbits;
    w->nbits += nbits;
    while (w->nbits >= 8) {
        bw_grow(w, 1);
        w->buf[w->pos++] = (uint8_t)(w->bits & 0xFF);
        w->bits >>= 8;
        w->nbits -= 8;
    }
}

void bw_flush(bit_writer_t *w) {
    if (w->nbits > 0) {
        bw_grow(w, 1);
        w->buf[w->pos++] = (uint8_t)(w->bits & 0xFF);
        w->bits = 0;
        w->nbits = 0;
    }
}

/* ── Reader ────────────────────────────────────────────────── */

void br_init(bit_reader_t *r, const uint8_t *buf, size_t len) {
    r->buf = buf;
    r->len = len;
    r->pos = 0;
    r->bits = 0;
    r->nbits = 0;
}

static void br_refill(bit_reader_t *r) {
    if (r->pos + 8 <= r->len) {
        /* Fast path: load 8 bytes in one shot */
        uint64_t raw;
        memcpy(&raw, r->buf + r->pos, 8);
        int shift = r->nbits;
        r->bits |= raw << shift;
        /* We loaded 8 bytes but can only use what fits in 64 bits */
        int consumed = (64 - shift) >> 3;  /* full bytes that fit */
        r->pos   += (size_t)consumed;
        r->nbits += consumed * 8;
    } else {
        /* Slow path near end of buffer */
        while (r->nbits <= 56 && r->pos < r->len) {
            r->bits |= (uint64_t)r->buf[r->pos++] << r->nbits;
            r->nbits += 8;
        }
    }
}

uint32_t br_peek(bit_reader_t *r, int nbits) {
    if (r->nbits < nbits) br_refill(r);
    return (uint32_t)(r->bits & ((1ULL << nbits) - 1));
}

uint32_t br_read(bit_reader_t *r, int nbits) {
    uint32_t val = br_peek(r, nbits);
    r->bits >>= nbits;
    r->nbits -= nbits;
    return val;
}
