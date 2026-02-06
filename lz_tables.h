#ifndef LZ_TABLES_H
#define LZ_TABLES_H

/*
 * DEFLATE-compatible length and distance coding tables.
 *
 * Lengths 3-258 are encoded as symbols 257-285 plus extra bits.
 * Distances 1-32768 are encoded as symbols 0-29 plus extra bits.
 */

#include <stdint.h>

#define LITLEN_SYMS   286   /* 0-255 literal, 256 end, 257-285 length */
#define LITLEN_END    256
#define DIST_SYMS     30
#define CODELEN_SYMS  19

/* ── Length codes (symbols 257-285) ────────────────────────── */

static const int base_length[29] = {
    3,4,5,6,7,8,9,10,  11,13,15,17,  19,23,27,31,
    35,43,51,59,  67,83,99,115,  131,163,195,227,  258
};

static const int extra_lbits[29] = {
    0,0,0,0,0,0,0,0,  1,1,1,1,  2,2,2,2,
    3,3,3,3,  4,4,4,4,  5,5,5,5,  0
};

/* ── Distance codes (symbols 0-29) ─────────────────────────── */

static const int base_dist[30] = {
    1,2,3,4,  5,7,9,13,  17,25,33,49,  65,97,129,193,
    257,385,513,769,  1025,1537,2049,3073,  4097,6145,8193,12289,  16385,24577
};

static const int extra_dbits[30] = {
    0,0,0,0,  1,1,2,2,  3,3,4,4,  5,5,6,6,
    7,7,8,8,  9,9,10,10,  11,11,12,12,  13,13
};

/* ── Code-length alphabet permutation ──────────────────────── */

static const int codelen_order[19] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
};

/* ── Encoding helpers ──────────────────────────────────────── */

/* Length (3-258) → symbol + extra bits */
static inline void len_to_code(int length, int *sym, int *ebits, int *eval) {
    for (int c = 28; c >= 0; c--) {
        if (length >= base_length[c]) {
            *sym = c + 257;
            *ebits = extra_lbits[c];
            *eval = length - base_length[c];
            return;
        }
    }
}

/* Distance (1-32768) → symbol + extra bits */
static inline void dist_to_code(int dist, int *sym, int *ebits, int *eval) {
    for (int c = 29; c >= 0; c--) {
        if (dist >= base_dist[c]) {
            *sym = c;
            *ebits = extra_dbits[c];
            *eval = dist - base_dist[c];
            return;
        }
    }
}

#endif
