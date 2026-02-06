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

#include "odz.h"
#include "bitstream.h"
#include "huffman.h"
#include "lz_tables.h"
#include "lz_matcher.h"

/* Raw LZ token: either a literal or a (length, distance) match */
typedef struct {
    uint16_t litlen;    /* literal byte (0-255) or match length (3-258) */
    uint16_t dist;      /* 0 = literal, >0 = match distance */
} token_t;

/* Compress one block of raw data into the bitstream buffer.
 * Returns the compressed data size. */
static size_t compress_block(const uint8_t *in, size_t n,
                             bit_writer_t *bw) {
    /* ── Pass 1: LZ77 → token buffer + frequency counts ──── */
    size_t max_tokens = n + 1; /* worst case: all literals + end symbol */
    token_t *tokens = malloc(max_tokens * sizeof(token_t));
    if (!tokens) die("oom");
    size_t ntok = 0;

    uint32_t ll_freq[LITLEN_SYMS] = {0};
    uint32_t d_freq[DIST_SYMS]    = {0};

    lz_matcher_t m;
    if (lz_matcher_init(&m, n, HASH_BITS, MAX_CHAIN_STEPS) != 0) die("oom");

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
            int lsym, lebits, leval;
            len_to_code(best_len, &lsym, &lebits, &leval);
            ll_freq[lsym]++;

            int dsym, debits, deval;
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
            bw_write(bw, ll_codes[s], ll_lens[s]);
        } else {
            /* Match */
            int lsym, lebits, leval;
            len_to_code(tokens[t].litlen, &lsym, &lebits, &leval);
            bw_write(bw, ll_codes[lsym], ll_lens[lsym]);
            if (lebits > 0) bw_write(bw, (uint32_t)leval, lebits);

            int dsym, debits, deval;
            dist_to_code(tokens[t].dist, &dsym, &debits, &deval);
            bw_write(bw, d_codes[dsym], d_lens[dsym]);
            if (debits > 0) bw_write(bw, (uint32_t)deval, debits);
        }
    }

    /* End-of-block */
    bw_write(bw, ll_codes[LITLEN_END], ll_lens[LITLEN_END]);
    bw_flush(bw);

    free(tokens);
    return bw->pos;
}

/* ── Public API ────────────────────────────────────────────── */

void odz_compress(FILE *in, FILE *out) {
    /* Get input size */
    if (fseeko(in, 0, SEEK_END) != 0) die("seek");
    int64_t in_size = ftello(in);
    if (in_size < 0) die("ftell");
    if (fseeko(in, 0, SEEK_SET) != 0) die("seek");

    /* Write file header: "ODZ" version(1) original_size(8) */
    uint8_t hdr[12];
    hdr[0] = 'O'; hdr[1] = 'D'; hdr[2] = 'Z'; hdr[3] = ODZ_VERSION;
    wr_u64le(hdr + 4, (uint64_t)in_size);
    if (fwrite(hdr, 1, 12, out) != 12) die("write header");

    uint8_t *block_buf = malloc(ODZ_BLOCK_SIZE);
    if (!block_buf) die("oom");

    uint64_t total_in = 0;
    uint64_t total_out = 12;

    int wrote_any = 0;
    for (;;) {
        size_t nread = fread(block_buf, 1, ODZ_BLOCK_SIZE, in);
        if (nread == 0) break;
        wrote_any = 1;

        int is_last = (total_in + nread >= (uint64_t)in_size);

        /* Try Huffman compression */
        bit_writer_t bw;
        bw_init(&bw, nread + 1024);
        size_t comp_size = compress_block(block_buf, nread, &bw);

        /* Block header: flags(1) + raw_size(4) */
        uint8_t blk_hdr[9];
        if (comp_size < nread) {
            /* Use compressed block */
            blk_hdr[0] = (uint8_t)((is_last ? 1 : 0) | (ODZ_BLOCK_HUFFMAN << 1));
            wr_u32le(blk_hdr + 1, (uint32_t)nread);
            wr_u32le(blk_hdr + 5, (uint32_t)comp_size);
            if (fwrite(blk_hdr, 1, 9, out) != 9) die("write block header");
            if (fwrite(bw.buf, 1, comp_size, out) != comp_size) die("write block");
            total_out += 9 + comp_size;
        } else {
            /* Stored block (compression didn't help) */
            blk_hdr[0] = (uint8_t)((is_last ? 1 : 0) | (ODZ_BLOCK_STORED << 1));
            wr_u32le(blk_hdr + 1, (uint32_t)nread);
            if (fwrite(blk_hdr, 1, 5, out) != 5) die("write block header");
            if (fwrite(block_buf, 1, nread, out) != nread) die("write block");
            total_out += 5 + nread;
        }

        bw_free(&bw);
        total_in += nread;

        /* Progress */
        fprintf(stderr, "\r  %llu / %llu bytes  (%.1f%%)",
                (unsigned long long)total_in,
                (unsigned long long)in_size,
                in_size > 0 ? 100.0 * total_in / in_size : 100.0);
    }

    /* Handle empty input: write one empty stored block */
    if (!wrote_any) {
        uint8_t blk_hdr[5];
        blk_hdr[0] = 1 | (ODZ_BLOCK_STORED << 1);  /* is_last + stored */
        wr_u32le(blk_hdr + 1, 0);
        if (fwrite(blk_hdr, 1, 5, out) != 5) die("write block header");
        total_out += 5;
    }

    free(block_buf);
    fprintf(stderr, "\n  %llu → %llu  (%.1f%%)\n",
            (unsigned long long)total_in,
            (unsigned long long)total_out,
            total_in > 0 ? 100.0 * total_out / total_in : 0.0);
}
