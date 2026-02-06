/*
 * Block-based LZ77 + Huffman decompressor.
 *
 * For each block:
 *   1. Read block header (type, raw size, compressed size)
 *   2. For stored blocks: copy raw data
 *   3. For Huffman blocks: read trees, decode tokens, replay LZ
 */

#include <stdlib.h>
#include <string.h>

#include "odz.h"
#include "bitstream.h"
#include "huffman.h"
#include "lz_tables.h"

/* Decode one symbol using two-level table */
static inline int huff_decode2(bit_reader_t *br,
                               const huff_decode_table_t *t) {
    uint32_t bits = br_peek(br, HUFF_MAX_BITS);
    huff_entry_t e = t->primary[bits & ((1 << HUFF_PRIMARY_BITS) - 1)];
    if ((e.len & 0x8000) == 0) {
        /* Primary hit (95%+ of cases) */
        br_consume(br, e.len);
        return e.sym;
    }
    /* Secondary lookup */
    int total_bits = e.len & 0x7FFF;
    int sub_idx = e.sym + (int)((bits >> HUFF_PRIMARY_BITS) &
                  ((1u << (total_bits - HUFF_PRIMARY_BITS)) - 1));
    huff_entry_t se = t->secondary[sub_idx];
    br_consume(br, se.len);
    return se.sym;
}

static void decompress_huffman_block(const uint8_t *comp, size_t comp_size,
                                     uint8_t *out, size_t raw_size,
                                     size_t *out_pos,
                                     huff_decode_table_t *ll_tab,
                                     huff_decode_table_t *d_tab) {
    bit_reader_t br;
    br_init(&br, comp, comp_size);

    /* Read Huffman trees */
    uint8_t ll_lens[LITLEN_SYMS], d_lens[DIST_SYMS];
    int n_ll, n_dist;
    huff_read_trees(&br, ll_lens, &n_ll, d_lens, &n_dist);

    /* Build two-level decode tables */
    huff_build_decode_table2(ll_lens, LITLEN_SYMS, ll_tab);
    huff_build_decode_table2(d_lens, DIST_SYMS, d_tab);

    /* Decode tokens */
    size_t op = *out_pos;
    for (;;) {
        int sym = huff_decode2(&br, ll_tab);

        if (sym < 256) {
            /* Literal */
            if (op >= raw_size) die("overrun");
            out[op++] = (uint8_t)sym;
        } else if (sym == LITLEN_END) {
            /* End of block */
            break;
        } else {
            /* Length code (257-285) */
            int code_idx = sym - 257;
            if (code_idx < 0 || code_idx >= 29) die("bad length code");
            int length = base_length[code_idx];
            if (extra_lbits[code_idx] > 0)
                length += (int)br_read(&br, extra_lbits[code_idx]);

            /* Distance code */
            int dcode = huff_decode2(&br, d_tab);
            if (dcode < 0 || dcode >= 30) die("bad distance code");
            int dist = base_dist[dcode];
            if (extra_dbits[dcode] > 0)
                dist += (int)br_read(&br, extra_dbits[dcode]);

            /* Copy match */
            if (dist <= 0 || (size_t)dist > op) die("bad distance");
            if (op + (size_t)length > raw_size) die("overrun");
            size_t src = op - (size_t)dist;
            if ((size_t)dist >= (size_t)length) {
                /* Non-overlapping: straight memcpy */
                memcpy(out + op, out + src, (size_t)length);
            } else if (dist == 1) {
                /* Byte fill (very common for runs) */
                memset(out + op, out[src], (size_t)length);
            } else {
                /* Overlapping: copy in dist-sized chunks */
                size_t rem = (size_t)length;
                size_t d = (size_t)dist;
                uint8_t *dst = out + op;
                const uint8_t *s = out + src;
                while (rem >= d) {
                    memcpy(dst, s, d);
                    dst += d;
                    rem -= d;
                }
                if (rem > 0) memcpy(dst, s, rem);
            }
            op += (size_t)length;
        }
    }
    *out_pos = op;
}

/* ── Public API ────────────────────────────────────────────── */

void odz_decompress(FILE *in, FILE *out) {
    /* Read file header */
    uint8_t hdr[12];
    if (fread(hdr, 1, 12, in) != 12) die("truncated header");
    if (hdr[0] != 'O' || hdr[1] != 'D' || hdr[2] != 'Z') die("bad magic");
    if (hdr[3] != ODZ_VERSION) die("unsupported version");

    uint64_t original_size = rd_u64le(hdr + 4);
    uint64_t total_out = 0;

    uint8_t *block_out = malloc(ODZ_BLOCK_SIZE);
    if (!block_out) die("oom");

    /* Allocate decode tables once, reuse across blocks */
    huff_decode_table_t ll_tab = {.secondary = NULL, .secondary_size = 0, .secondary_cap = 0};
    huff_decode_table_t d_tab  = {.secondary = NULL, .secondary_size = 0, .secondary_cap = 0};

    for (;;) {
        /* Read block header */
        uint8_t blk_hdr[9];
        if (fread(blk_hdr, 1, 1, in) != 1) die("truncated block header");

        int is_last  = blk_hdr[0] & 1;
        int blk_type = (blk_hdr[0] >> 1) & 3;

        if (blk_type == ODZ_BLOCK_STORED) {
            /* Read raw_size */
            if (fread(blk_hdr + 1, 1, 4, in) != 4) die("truncated");
            uint32_t raw_size = rd_u32le(blk_hdr + 1);
            if (raw_size > ODZ_BLOCK_SIZE) die("block too large");

            /* Read and write raw data */
            if (fread(block_out, 1, raw_size, in) != raw_size) die("truncated");
            if (fwrite(block_out, 1, raw_size, out) != raw_size) die("write");
            total_out += raw_size;

        } else if (blk_type == ODZ_BLOCK_HUFFMAN) {
            /* Read raw_size + compressed_size */
            if (fread(blk_hdr + 1, 1, 8, in) != 8) die("truncated");
            uint32_t raw_size  = rd_u32le(blk_hdr + 1);
            uint32_t comp_size = rd_u32le(blk_hdr + 5);
            if (raw_size > ODZ_BLOCK_SIZE) die("block too large");

            /* Read compressed data */
            uint8_t *comp = malloc(comp_size);
            if (!comp) die("oom");
            if (fread(comp, 1, comp_size, in) != comp_size) die("truncated");

            /* Decompress */
            size_t out_pos = 0;
            decompress_huffman_block(comp, comp_size,
                                     block_out, raw_size, &out_pos,
                                     &ll_tab, &d_tab);
            if (out_pos != raw_size) die("size mismatch in block");

            if (fwrite(block_out, 1, raw_size, out) != raw_size) die("write");
            total_out += raw_size;
            free(comp);
        } else {
            die("unknown block type");
        }

        fprintf(stderr, "\r  %llu / %llu bytes",
                (unsigned long long)total_out,
                (unsigned long long)original_size);

        if (is_last) break;
    }

    if (total_out != original_size) die("total size mismatch");

    huff_free_decode_table2(&ll_tab);
    huff_free_decode_table2(&d_tab);
    free(block_out);
    fprintf(stderr, "\n  decompressed %llu bytes\n",
            (unsigned long long)total_out);
}
