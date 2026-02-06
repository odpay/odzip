#ifndef HUFFMAN_H
#define HUFFMAN_H

#include <stdint.h>
#include "bitstream.h"
#include "lz_tables.h"

#define HUFF_MAX_BITS     15   /* max code length for lit/len and distance */
#define HUFF_CL_MAX_BITS  7    /* max code length for the code-length alphabet */
#define HUFF_PRIMARY_BITS 9    /* primary table bits for two-level decode */

/* Decode table entry — used for fast table-based decoding */
typedef struct {
    uint16_t sym;
    uint16_t len;   /* bits consumed */
} huff_entry_t;

/* Two-level decode table: 9-bit primary + secondary overflow */
typedef struct {
    huff_entry_t primary[1 << HUFF_PRIMARY_BITS];  /* 512 entries = 2KB */
    huff_entry_t *secondary;                         /* overflow sub-tables */
    int           secondary_size;
    int           secondary_cap;
} huff_decode_table_t;

/*
 * Build canonical Huffman code lengths from symbol frequencies.
 * freqs[0..nsym-1]: frequency of each symbol (0 = unused).
 * max_bits: maximum allowed code length.
 * out[0..nsym-1]: resulting code lengths (0 = symbol not in code).
 */
void huff_build_lengths(const uint32_t *freqs, int nsym, int max_bits,
                        uint8_t *out);

/*
 * Build bit-reversed canonical Huffman codes from code lengths.
 * The codes are reversed so they can be fed directly to bw_write (LSB-first).
 */
void huff_build_codes(const uint8_t *lengths, int nsym,
                      uint16_t *codes);

/*
 * Build a flat decode table from code lengths.
 * table must have (1 << table_bits) entries.
 * table_bits should be >= max code length present.
 */
void huff_build_decode_table(const uint8_t *lengths, int nsym,
                             huff_entry_t *table, int table_bits);

/*
 * Build a two-level decode table (9-bit primary + secondary overflow).
 * Primary table is stack-allocated inside the struct; secondary is heap-allocated.
 * Call huff_free_decode_table2 when done, or reuse by calling build again.
 */
void huff_build_decode_table2(const uint8_t *lengths, int nsym,
                              huff_decode_table_t *t);
void huff_free_decode_table2(huff_decode_table_t *t);

/*
 * Write lit/len + distance Huffman trees to the bitstream
 * using the DEFLATE 3-level code-length encoding.
 */
void huff_write_trees(bit_writer_t *bw,
                      const uint8_t *ll_lens, int n_ll,
                      const uint8_t *d_lens, int n_dist);

/*
 * Read lit/len + distance Huffman trees from the bitstream.
 */
void huff_read_trees(bit_reader_t *br,
                     uint8_t *ll_lens, int *n_ll,
                     uint8_t *d_lens, int *n_dist);

#endif
