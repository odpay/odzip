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

#include "libodzip.h"
#include "odz.h"
#include "bitstream.h"
#include "huffman.h"
#include "lz_tables.h"

#include "odz_thread.h"

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

/* Returns ODZ_OK on success, ODZ_ERR_* on failure */
static int decompress_huffman_block(const uint8_t *comp, size_t comp_size,
                                    uint8_t *out, size_t raw_size,
                                    size_t *out_pos,
                                    huff_decode_table_t *ll_tab,
                                    huff_decode_table_t *d_tab) {
    bit_reader_t br;
    br_init(&br, comp, comp_size);

    /* Read Huffman trees */
    uint8_t ll_lens[LITLEN_SYMS], d_lens[DIST_SYMS];
    int n_ll, n_dist;
    if (huff_read_trees(&br, ll_lens, &n_ll, d_lens, &n_dist) != 0)
        return ODZ_ERR_CORRUPT;

    /* Build two-level decode tables */
    if (huff_build_decode_table2(ll_lens, LITLEN_SYMS, ll_tab) != 0)
        return ODZ_ERR_OOM;
    if (huff_build_decode_table2(d_lens, DIST_SYMS, d_tab) != 0)
        return ODZ_ERR_OOM;

    /* Decode tokens */
    size_t op = *out_pos;
    for (;;) {
        int sym = huff_decode2(&br, ll_tab);

        if (sym < 256) {
            /* Literal */
            if (op >= raw_size) return ODZ_ERR_CORRUPT;
            out[op++] = (uint8_t)sym;
        } else if (sym == LITLEN_END) {
            /* End of block */
            break;
        } else {
            /* Length code (257-285) */
            int code_idx = sym - 257;
            if (code_idx < 0 || code_idx >= 29) return ODZ_ERR_CORRUPT;
            int length = base_length[code_idx];
            if (extra_lbits[code_idx] > 0)
                length += (int)br_read(&br, extra_lbits[code_idx]);

            /* Distance code */
            int dcode = huff_decode2(&br, d_tab);
            if (dcode < 0 || dcode >= 30) return ODZ_ERR_CORRUPT;
            int dist = base_dist[dcode];
            if (extra_dbits[dcode] > 0)
                dist += (int)br_read(&br, extra_dbits[dcode]);

            /* Copy match */
            if (dist <= 0 || (size_t)dist > op) return ODZ_ERR_CORRUPT;
            if (op + (size_t)length > raw_size) return ODZ_ERR_CORRUPT;
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
    return ODZ_OK;
}

/* ── Parallel block decompression ──────────────────────────── */

typedef struct {
    int blk_type;
    uint8_t *comp;
    size_t comp_size;
    uint8_t *out;
    size_t raw_size;
    size_t out_pos;
    int err;
    huff_decode_table_t ll_tab;
    huff_decode_table_t d_tab;
} decomp_job_t;

static void *decomp_worker(void *arg) {
    decomp_job_t *j = arg;
    if (j->blk_type == ODZ_BLOCK_STORED) {
        if (j->raw_size > 0)
            memcpy(j->out, j->comp, j->raw_size);
        j->out_pos = j->raw_size;
        j->err = ODZ_OK;
    } else {
        j->out_pos = 0;
        j->err = decompress_huffman_block(j->comp, j->comp_size,
                                           j->out, j->raw_size, &j->out_pos,
                                           &j->ll_tab, &j->d_tab);
    }
    return NULL;
}

/* ── Public API ────────────────────────────────────────────── */

int odz_decompress(FILE *in, FILE *out, const odz_options_t *opts) {
    int rc = ODZ_OK;
    int nthreads = (opts && opts->threads > 1) ? opts->threads : 1;

    /* Read file header */
    uint8_t hdr[12];
    if (fread(hdr, 1, 12, in) != 12) return ODZ_ERR_IO;
    if (hdr[0] != 'O' || hdr[1] != 'D' || hdr[2] != 'Z') return ODZ_ERR_FORMAT;
    if (hdr[3] != ODZ_VERSION) return ODZ_ERR_FORMAT;

    uint64_t original_size = rd_u64le(hdr + 4);
    uint64_t total_out = 0;

    /* Per-thread resources */
    uint8_t **out_bufs = calloc(nthreads, sizeof(uint8_t *));
    decomp_job_t *jobs = calloc(nthreads, sizeof(decomp_job_t));
    odz_thread_t *tids = (nthreads > 1) ? malloc(nthreads * sizeof(odz_thread_t)) : NULL;
    if (!out_bufs || !jobs || (nthreads > 1 && !tids)) {
        rc = ODZ_ERR_OOM; goto cleanup;
    }
    for (int i = 0; i < nthreads; i++) {
        out_bufs[i] = malloc(ODZ_BLOCK_SIZE);
        if (!out_bufs[i]) { rc = ODZ_ERR_OOM; goto cleanup; }
    }

    for (;;) {
        /* Read batch of blocks */
        int nblocks = 0;
        int saw_last = 0;

        for (int i = 0; i < nthreads && !saw_last; i++) {
            uint8_t flags;
            if (fread(&flags, 1, 1, in) != 1) { rc = ODZ_ERR_IO; goto cleanup; }

            int is_last  = flags & 1;
            int blk_type = (flags >> 1) & 3;
            uint8_t blk_hdr[8];

            jobs[i].blk_type = blk_type;
            jobs[i].out = out_bufs[i];
            jobs[i].err = ODZ_OK;
            jobs[i].comp = NULL;

            if (blk_type == ODZ_BLOCK_STORED) {
                if (fread(blk_hdr, 1, 4, in) != 4) { rc = ODZ_ERR_IO; goto cleanup; }
                uint32_t raw_size = rd_u32le(blk_hdr);
                if (raw_size > ODZ_BLOCK_SIZE) { rc = ODZ_ERR_CORRUPT; goto cleanup; }

                jobs[i].raw_size = raw_size;
                jobs[i].comp_size = 0;
                if (raw_size > 0) {
                    jobs[i].comp = malloc(raw_size);
                    if (!jobs[i].comp) { rc = ODZ_ERR_OOM; goto cleanup; }
                    if (fread(jobs[i].comp, 1, raw_size, in) != raw_size) {
                        rc = ODZ_ERR_IO; goto cleanup;
                    }
                }
            } else if (blk_type == ODZ_BLOCK_HUFFMAN) {
                if (fread(blk_hdr, 1, 8, in) != 8) { rc = ODZ_ERR_IO; goto cleanup; }
                uint32_t raw_size  = rd_u32le(blk_hdr);
                uint32_t comp_size = rd_u32le(blk_hdr + 4);
                if (raw_size > ODZ_BLOCK_SIZE || comp_size > ODZ_BLOCK_SIZE) {
                    rc = ODZ_ERR_CORRUPT; goto cleanup;
                }

                jobs[i].raw_size  = raw_size;
                jobs[i].comp_size = comp_size;
                jobs[i].comp = malloc(comp_size);
                if (!jobs[i].comp) { rc = ODZ_ERR_OOM; goto cleanup; }
                if (fread(jobs[i].comp, 1, comp_size, in) != comp_size) {
                    rc = ODZ_ERR_IO; goto cleanup;
                }
            } else {
                rc = ODZ_ERR_FORMAT; goto cleanup;
            }

            nblocks++;
            if (is_last) saw_last = 1;
        }

        if (nblocks == 0) break;

        /* Decompress -- main thread takes [0], spawn the rest */
        for (int i = 1; i < nblocks; i++) {
            if (odz_thread_create(&tids[i], decomp_worker, &jobs[i]) != 0) {
                tids[i] = ODZ_THREAD_NULL;
                decomp_worker(&jobs[i]);
            }
        }
        decomp_worker(&jobs[0]);
        for (int i = 1; i < nblocks; i++)
            if (tids[i]) odz_thread_join(tids[i]);

        /* Write results in order */
        for (int i = 0; i < nblocks; i++) {
            if (jobs[i].err) { rc = jobs[i].err; goto cleanup; }
            if (jobs[i].blk_type == ODZ_BLOCK_HUFFMAN &&
                jobs[i].out_pos != jobs[i].raw_size) {
                rc = ODZ_ERR_CORRUPT; goto cleanup;
            }

            size_t n = (jobs[i].blk_type == ODZ_BLOCK_STORED)
                     ? jobs[i].raw_size : jobs[i].out_pos;
            if (fwrite(jobs[i].out, 1, n, out) != n) {
                rc = ODZ_ERR_IO; goto cleanup;
            }
            total_out += n;

            free(jobs[i].comp);
            jobs[i].comp = NULL;

            if (opts && opts->progress)
                opts->progress(total_out, original_size, opts->userdata);
        }

        if (saw_last) break;
    }

    if (total_out != original_size) rc = ODZ_ERR_CORRUPT;

cleanup:
    if (jobs) {
        for (int i = 0; i < nthreads; i++) {
            free(jobs[i].comp);
            huff_free_decode_table2(&jobs[i].ll_tab);
            huff_free_decode_table2(&jobs[i].d_tab);
        }
    }
    if (out_bufs)
        for (int i = 0; i < nthreads; i++) free(out_bufs[i]);
    free(out_bufs);
    free(jobs);
    free(tids);
    return rc;
}
