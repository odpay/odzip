/*
 * Block-based LZ77 + Huffman compressor.
 *
 * For each 1 MB block:
 *   1. Run LZ77 hash-chain matcher → token buffer
 *   2. Count symbol frequencies, build Huffman trees
 *   3. Write Huffman trees + encoded tokens to bitstream buffer
 *   4. Write block header + compressed data to output
 */

#include <stdlib.h>
#include <string.h>

#ifdef _MSC_VER
#define fseeko _fseeki64
#define ftello _ftelli64
#endif

#include "libodzip.h"
#include "odz.h"
#include "bitstream.h"
#include "huffman.h"
#include "lz_tables.h"
#include "lz_matcher.h"

#include "odz_thread.h"

/* Raw LZ token: either a literal or a (length, distance) match */
typedef struct {
    uint16_t litlen;    /* literal byte (0-255) or match length (3-258) */
    uint16_t dist;      /* 0 = literal, >0 = match distance */
} token_t;

/* Compress one block of raw data into the bitstream buffer.
 * Returns the compressed data size, or 0 on error (sets *err). */
static size_t compress_block(const uint8_t *in, size_t n,
                             bit_writer_t *bw, int *err) {
    *err = 0;

    /* ── Pass 1: LZ77 → token buffer + frequency counts ──── */
    size_t max_tokens = n + 1; /* worst case: all literals + end symbol */
    token_t *tokens = malloc(max_tokens * sizeof(token_t));
    if (!tokens) { *err = ODZ_ERR_OOM; return 0; }
    size_t ntok = 0;

    uint32_t ll_freq[LITLEN_SYMS] = {0};
    uint32_t d_freq[DIST_SYMS]    = {0};

    lz_matcher_t m;
    if (lz_matcher_init(&m, n, HASH_BITS, MAX_CHAIN_STEPS) != 0) {
        free(tokens);
        *err = ODZ_ERR_OOM;
        return 0;
    }

    size_t i = 0;
    while (i < n) {
        int best_len = 0, best_dist = 0;
        lz_matcher_find_best(&m, in, i, n, (int)ODZ_WINDOW,
                             ODZ_MIN_MATCH, ODZ_MAX_MATCH,
                             &best_len, &best_dist);

        /* Lazy matching: check if the next position has a longer match.
         * Skip the check for near-maximum matches (not worth it). */
        if (best_len >= ODZ_MIN_MATCH && best_len < ODZ_MAX_MATCH - 1 && i + 1 < n) {
            lz_matcher_insert(&m, in, i);
            int next_len = 0, next_dist = 0;
            lz_matcher_find_best_next(&m, in, i, n, (int)ODZ_WINDOW,
                                      ODZ_MIN_MATCH, ODZ_MAX_MATCH,
                                      &next_len, &next_dist);
            if (next_len > best_len) {
                /* Emit literal, take the longer match next time */
                ll_freq[in[i]]++;
                tokens[ntok].litlen = in[i];
                tokens[ntok].dist = 0;
                ntok++; i++;
                continue;
            }
        }

        if (best_len >= ODZ_MIN_MATCH) {
            /* Emit match token */
            int lsym = 0, lebits = 0, leval = 0;
            len_to_code(best_len, &lsym, &lebits, &leval);
            ll_freq[lsym]++;

            int dsym = 0, debits = 0, deval = 0;
            dist_to_code(best_dist, &dsym, &debits, &deval);
            d_freq[dsym]++;

            tokens[ntok].litlen = (uint16_t)best_len;
            tokens[ntok].dist   = (uint16_t)best_dist;
            ntok++;

            /* Insert ALL positions covered by the match */
            for (size_t p = i; p < i + (size_t)best_len && p + 2 < n; p++)
                lz_matcher_insert(&m, in, p);
            i += (size_t)best_len;
        } else {
            /* Emit literal */
            lz_matcher_insert(&m, in, i);
            ll_freq[in[i]]++;
            tokens[ntok].litlen = in[i];
            tokens[ntok].dist = 0;
            ntok++; i++;
        }
    }
    lz_matcher_free(&m);

    /* End-of-block symbol */
    ll_freq[LITLEN_END]++;

    /* Ensure at least one distance symbol exists (for valid tree) */
    if (d_freq[0] == 0) {
        int any = 0;
        for (int s = 0; s < DIST_SYMS; s++) if (d_freq[s]) { any = 1; break; }
        if (!any) d_freq[0] = 1;
    }

    /* ── Build Huffman trees ─────────────────────────────── */
    uint8_t  ll_lens[LITLEN_SYMS], d_lens[DIST_SYMS];
    uint16_t ll_codes[LITLEN_SYMS], d_codes[DIST_SYMS];

    huff_build_lengths(ll_freq, LITLEN_SYMS, HUFF_MAX_BITS, ll_lens);
    huff_build_lengths(d_freq, DIST_SYMS, HUFF_MAX_BITS, d_lens);
    huff_build_codes(ll_lens, LITLEN_SYMS, ll_codes);
    huff_build_codes(d_lens, DIST_SYMS, d_codes);

    /* ── Pass 2: write trees + encoded tokens to bitstream ── */
    huff_write_trees(bw, ll_lens, LITLEN_SYMS, d_lens, DIST_SYMS);

    for (size_t t = 0; t < ntok; t++) {
        if (tokens[t].dist == 0) {
            /* Literal */
            int s = tokens[t].litlen;
            if (bw_write(bw, ll_codes[s], ll_lens[s]) != 0) goto oom;
        } else {
            /* Match */
            int lsym = 0, lebits = 0, leval = 0;
            len_to_code(tokens[t].litlen, &lsym, &lebits, &leval);
            if (bw_write(bw, ll_codes[lsym], ll_lens[lsym]) != 0) goto oom;
            if (lebits > 0 && bw_write(bw, (uint32_t)leval, lebits) != 0) goto oom;

            int dsym = 0, debits = 0, deval = 0;
            dist_to_code(tokens[t].dist, &dsym, &debits, &deval);
            if (bw_write(bw, d_codes[dsym], d_lens[dsym]) != 0) goto oom;
            if (debits > 0 && bw_write(bw, (uint32_t)deval, debits) != 0) goto oom;
        }
    }

    /* End-of-block */
    if (bw_write(bw, ll_codes[LITLEN_END], ll_lens[LITLEN_END]) != 0) goto oom;
    if (bw_flush(bw) != 0) goto oom;

    free(tokens);
    return bw->pos;

oom:
    free(tokens);
    *err = ODZ_ERR_OOM;
    return 0;
}

/* ── Parallel block compression ────────────────────────────── */

typedef struct {
    const uint8_t *in;
    size_t in_size;
    bit_writer_t bw;
    size_t comp_size;
    int err;
} comp_job_t;

static void *comp_worker(void *arg) {
    comp_job_t *j = arg;
    if (bw_init(&j->bw, j->in_size + 1024) != 0) {
        j->err = ODZ_ERR_OOM;
        return NULL;
    }
    j->comp_size = compress_block(j->in, j->in_size, &j->bw, &j->err);
    return NULL;
}

/* ── Public API ────────────────────────────────────────────── */

int odz_compress(FILE *in, FILE *out, const odz_options_t *opts) {
    int rc = ODZ_OK;
    int nthreads = (opts && opts->threads > 1) ? opts->threads : 1;

    /* Get input size */
    if (fseeko(in, 0, SEEK_END) != 0) return ODZ_ERR_IO;
    int64_t in_size = ftello(in);
    if (in_size < 0) return ODZ_ERR_IO;
    if (fseeko(in, 0, SEEK_SET) != 0) return ODZ_ERR_IO;

    /* Write file header: "ODZ" version(1) original_size(8) */
    uint8_t hdr[12];
    hdr[0] = 'O'; hdr[1] = 'D'; hdr[2] = 'Z'; hdr[3] = ODZ_VERSION;
    wr_u64le(hdr + 4, (uint64_t)in_size);
    if (fwrite(hdr, 1, 12, out) != 12) return ODZ_ERR_IO;

    /* Per-thread resources */
    uint8_t **blk_bufs = calloc(nthreads, sizeof(uint8_t *));
    comp_job_t *jobs = calloc(nthreads, sizeof(comp_job_t));
    odz_thread_t *tids = (nthreads > 1) ? malloc(nthreads * sizeof(odz_thread_t)) : NULL;
    if (!blk_bufs || !jobs || (nthreads > 1 && !tids)) {
        rc = ODZ_ERR_OOM; goto cleanup;
    }
    for (int i = 0; i < nthreads; i++) {
        blk_bufs[i] = malloc(ODZ_BLOCK_SIZE);
        if (!blk_bufs[i]) { rc = ODZ_ERR_OOM; goto cleanup; }
    }

    uint64_t total_in = 0;
    int wrote_any = 0;

    for (;;) {
        /* Read batch of blocks */
        int nblocks = 0;
        for (int i = 0; i < nthreads; i++) {
            size_t nread = fread(blk_bufs[i], 1, ODZ_BLOCK_SIZE, in);
            if (nread == 0) break;
            jobs[i].in = blk_bufs[i];
            jobs[i].in_size = nread;
            jobs[i].err = 0;
            nblocks++;
        }
        if (nblocks == 0) break;
        wrote_any = 1;

        /* Compress -- main thread takes [0], spawn the rest */
        for (int i = 1; i < nblocks; i++) {
            if (odz_thread_create(&tids[i], comp_worker, &jobs[i]) != 0) {
                tids[i] = ODZ_THREAD_NULL;
                comp_worker(&jobs[i]);
            }
        }
        comp_worker(&jobs[0]);
        for (int i = 1; i < nblocks; i++)
            if (tids[i]) odz_thread_join(tids[i]);

        /* Write results in order */
        for (int i = 0; i < nblocks; i++) {
            if (jobs[i].err) { rc = jobs[i].err; goto cleanup; }
            total_in += jobs[i].in_size;
            int is_last = (total_in >= (uint64_t)in_size);

            uint8_t blk_hdr[9];
            if (jobs[i].comp_size < jobs[i].in_size) {
                blk_hdr[0] = (uint8_t)((is_last ? 1 : 0) | (ODZ_BLOCK_HUFFMAN << 1));
                wr_u32le(blk_hdr + 1, (uint32_t)jobs[i].in_size);
                wr_u32le(blk_hdr + 5, (uint32_t)jobs[i].comp_size);
                if (fwrite(blk_hdr, 1, 9, out) != 9 ||
                    fwrite(jobs[i].bw.buf, 1, jobs[i].comp_size, out) != jobs[i].comp_size) {
                    rc = ODZ_ERR_IO; goto cleanup;
                }
            } else {
                blk_hdr[0] = (uint8_t)((is_last ? 1 : 0) | (ODZ_BLOCK_STORED << 1));
                wr_u32le(blk_hdr + 1, (uint32_t)jobs[i].in_size);
                if (fwrite(blk_hdr, 1, 5, out) != 5 ||
                    fwrite(blk_bufs[i], 1, jobs[i].in_size, out) != jobs[i].in_size) {
                    rc = ODZ_ERR_IO; goto cleanup;
                }
            }
            bw_free(&jobs[i].bw);

            if (opts && opts->progress)
                opts->progress(total_in, (uint64_t)in_size, opts->userdata);
        }
    }

    /* Handle empty input: write one empty stored block */
    if (!wrote_any) {
        uint8_t blk_hdr[5];
        blk_hdr[0] = 1 | (ODZ_BLOCK_STORED << 1);
        wr_u32le(blk_hdr + 1, 0);
        if (fwrite(blk_hdr, 1, 5, out) != 5) { rc = ODZ_ERR_IO; goto cleanup; }
    }

cleanup:
    if (jobs)
        for (int i = 0; i < nthreads; i++) bw_free(&jobs[i].bw);
    if (blk_bufs)
        for (int i = 0; i < nthreads; i++) free(blk_bufs[i]);
    free(blk_bufs);
    free(jobs);
    free(tids);
    return rc;
}
