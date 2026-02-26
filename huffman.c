#include "huffman.h"
#include <stdlib.h>
#include <string.h>

/* ── Bit reversal ──────────────────────────────────────────── */

static uint16_t bit_reverse(uint16_t code, int len) {
    uint16_t r = 0;
    for (int i = 0; i < len; i++) {
        r = (r << 1) | (code & 1);
        code >>= 1;
    }
    return r;
}

/* ── Build code lengths (Huffman tree) ─────────────────────── */

/*
 * Two-queue Huffman tree builder.
 * Sorts non-zero-frequency symbols, merges bottom-up,
 * extracts depths, then length-limits if needed.
 */

typedef struct { int sym; uint32_t freq; } sf_t;

static int sf_cmp(const void *a, const void *b) {
    const sf_t *x = a, *y = b;
    if (x->freq != y->freq) return (x->freq < y->freq) ? -1 : 1;
    return (x->sym < y->sym) ? -1 : 1;
}

static void limit_lengths(uint8_t *lengths, int nsym, int max_bits) {
    /* Check if limiting is needed */
    int overflow = 0;
    for (int i = 0; i < nsym; i++)
        if (lengths[i] > max_bits) overflow = 1;
    if (!overflow) return;

    /* Cap all lengths */
    for (int i = 0; i < nsym; i++)
        if (lengths[i] > max_bits) lengths[i] = (uint8_t)max_bits;

    /* Compute kraft sum: sum of 2^(max_bits - len[i]) for active symbols */
    int32_t kraft = 0;
    for (int i = 0; i < nsym; i++)
        if (lengths[i] > 0)
            kraft += 1 << (max_bits - lengths[i]);
    int32_t target = 1 << max_bits;

    /* Oversubscribed: lengthen codes starting from longest available < max_bits */
    while (kraft > target) {
        for (int d = max_bits - 1; d >= 1; d--) {
            /* Find any symbol at this length */
            for (int i = 0; i < nsym; i++) {
                if (lengths[i] == d) {
                    kraft -= 1 << (max_bits - d);
                    lengths[i]++;
                    kraft += 1 << (max_bits - lengths[i]);
                    goto check_kraft;
                }
            }
        }
        break; /* can't fix further */
check_kraft:
        ;
    }
}

void huff_build_lengths(const uint32_t *freqs, int nsym, int max_bits,
                        uint8_t *out) {
    memset(out, 0, (size_t)nsym);

    /* Collect active symbols */
    sf_t sf[LITLEN_SYMS];  /* max alphabet size */
    int na = 0;
    for (int i = 0; i < nsym; i++)
        if (freqs[i] > 0) { sf[na].sym = i; sf[na].freq = freqs[i]; na++; }

    if (na == 0) return;
    if (na == 1) { out[sf[0].sym] = 1; return; }
    if (na == 2) { out[sf[0].sym] = 1; out[sf[1].sym] = 1; return; }

    /* Sort by frequency ascending */
    qsort(sf, (size_t)na, sizeof(sf_t), sf_cmp);

    /* Build tree with two-queue method.
     * Nodes 0..na-1 = leaves (sorted).  Nodes na..2*na-2 = internal. */
    int total = 2 * na - 1;
    uint32_t nfreq[LITLEN_SYMS * 2];
    int16_t  npar[LITLEN_SYMS * 2];

    for (int i = 0; i < na; i++) { nfreq[i] = sf[i].freq; npar[i] = -1; }

    int lf = 0;                     /* leaf front */
    int ib = na;                    /* internal back (write position) */
    int it = na;                    /* internal front (read position) */

    for (int i = na; i < total; i++) {
        /* Pick two smallest */
        int a, b;
        if (it >= ib || (lf < na && nfreq[lf] <= nfreq[it]))
            a = lf++;
        else
            a = it++;
        if (it >= ib || (lf < na && nfreq[lf] <= nfreq[it]))
            b = lf++;
        else
            b = it++;

        nfreq[i] = nfreq[a] + nfreq[b];
        npar[a] = (int16_t)i;
        npar[b] = (int16_t)i;
        npar[i] = -1;
        ib = i + 1;
    }

    /* Extract depths */
    for (int i = 0; i < na; i++) {
        int depth = 0;
        for (int j = i; npar[j] != -1; j = npar[j]) depth++;
        out[sf[i].sym] = (uint8_t)depth;
    }

    /* Length-limit */
    limit_lengths(out, nsym, max_bits);
}

/* ── Build canonical codes from lengths ────────────────────── */

void huff_build_codes(const uint8_t *lengths, int nsym, uint16_t *codes) {
    int bl_count[HUFF_MAX_BITS + 1] = {0};
    for (int i = 0; i < nsym; i++)
        if (lengths[i] > 0) bl_count[lengths[i]]++;

    uint16_t next_code[HUFF_MAX_BITS + 1];
    next_code[0] = 0;
    for (int b = 1; b <= HUFF_MAX_BITS; b++)
        next_code[b] = (next_code[b - 1] + bl_count[b - 1]) << 1;

    for (int i = 0; i < nsym; i++) {
        if (lengths[i] > 0)
            codes[i] = bit_reverse(next_code[lengths[i]]++, lengths[i]);
        else
            codes[i] = 0;
    }
}

/* ── Build flat decode table ───────────────────────────────── */

void huff_build_decode_table(const uint8_t *lengths, int nsym,
                             huff_entry_t *table, int table_bits) {
    int table_size = 1 << table_bits;

    /* Default: invalid */
    for (int i = 0; i < table_size; i++) {
        table[i].sym = 0xFFFF;
        table[i].len = (uint16_t)table_bits;
    }

    uint16_t codes[LITLEN_SYMS];
    huff_build_codes(lengths, nsym, codes);

    for (int s = 0; s < nsym; s++) {
        if (lengths[s] == 0) continue;
        int len = lengths[s];
        uint16_t code = codes[s];  /* already bit-reversed */
        int fill = 1 << (table_bits - len);
        for (int j = 0; j < fill; j++) {
            int idx = code | (j << len);
            table[idx].sym = (uint16_t)s;
            table[idx].len = (uint16_t)len;
        }
    }
}

/* ── Two-level decode table (9-bit primary + overflow) ─────── */

int huff_build_decode_table2(const uint8_t *lengths, int nsym,
                              huff_decode_table_t *t) {
    const int pbits = HUFF_PRIMARY_BITS;
    const int psize = 1 << pbits;

    /* Default primary: invalid */
    for (int i = 0; i < psize; i++) {
        t->primary[i].sym = 0xFFFF;
        t->primary[i].len = (uint16_t)pbits;
    }

    /* Build canonical codes */
    uint16_t codes[LITLEN_SYMS];
    huff_build_codes(lengths, nsym, codes);

    /* Find max code length to know if we need secondary tables */
    int max_len = 0;
    for (int s = 0; s < nsym; s++)
        if (lengths[s] > max_len) max_len = lengths[s];

    /* First pass: fill primary table for codes <= pbits */
    for (int s = 0; s < nsym; s++) {
        if (lengths[s] == 0 || lengths[s] > pbits) continue;
        int len = lengths[s];
        uint16_t code = codes[s];
        int fill = 1 << (pbits - len);
        for (int j = 0; j < fill; j++) {
            int idx = code | (j << len);
            t->primary[idx].sym = (uint16_t)s;
            t->primary[idx].len = (uint16_t)len;
        }
    }

    /* If no codes exceed primary bits, no secondary needed */
    if (max_len <= pbits) {
        t->secondary_size = 0;
        return 0;
    }

    /* Second pass: build secondary sub-tables for codes > pbits.
     * For each unique primary prefix, we create a sub-table.
     * Primary entry stores: sym = offset into secondary, len = (pbits + sub_bits) | 0x8000 flag */

    /* Count how many secondary entries we need */
    /* Group long codes by their primary (bottom pbits) prefix */
    /* For each prefix group, the sub-table needs 2^(max_sub_len - pbits) entries */

    /* First, find which primary slots have overflow and their max lengths */
    int prefix_max_len[1 << HUFF_PRIMARY_BITS];
    memset(prefix_max_len, 0, sizeof(prefix_max_len));
    for (int s = 0; s < nsym; s++) {
        if (lengths[s] <= pbits) continue;
        int prefix = codes[s] & (psize - 1);
        if (lengths[s] > prefix_max_len[prefix])
            prefix_max_len[prefix] = lengths[s];
    }

    /* Calculate total secondary size and assign offsets */
    int sec_total = 0;
    int prefix_offset[1 << HUFF_PRIMARY_BITS];
    int prefix_sub_bits[1 << HUFF_PRIMARY_BITS];
    for (int p = 0; p < psize; p++) {
        if (prefix_max_len[p] == 0) {
            prefix_offset[p] = -1;
            prefix_sub_bits[p] = 0;
            continue;
        }
        int sub_bits = prefix_max_len[p] - pbits;
        prefix_sub_bits[p] = sub_bits;
        prefix_offset[p] = sec_total;
        sec_total += 1 << sub_bits;
    }

    /* Allocate or reuse secondary array */
    if (sec_total > t->secondary_cap) {
        free(t->secondary);
        t->secondary = malloc((size_t)sec_total * sizeof(huff_entry_t));
        if (!t->secondary) return -1;
        t->secondary_cap = sec_total;
    }
    t->secondary_size = sec_total;

    /* Default secondary entries to invalid */
    for (int i = 0; i < sec_total; i++) {
        t->secondary[i].sym = 0xFFFF;
        t->secondary[i].len = HUFF_MAX_BITS;
    }

    /* Fill secondary sub-tables */
    for (int s = 0; s < nsym; s++) {
        if (lengths[s] <= pbits) continue;
        int len = lengths[s];
        uint16_t code = codes[s];
        int prefix = code & (psize - 1);
        int sub_code = code >> pbits;
        int sub_bits = prefix_sub_bits[prefix];
        int sub_len = len - pbits;
        int fill = 1 << (sub_bits - sub_len);
        int base = prefix_offset[prefix];
        for (int j = 0; j < fill; j++) {
            int idx = base + (sub_code | (j << sub_len));
            t->secondary[idx].sym = (uint16_t)s;
            t->secondary[idx].len = (uint16_t)len;
        }
    }

    /* Update primary entries for overflow prefixes:
     * sym = offset into secondary, len with high bit set as flag */
    for (int p = 0; p < psize; p++) {
        if (prefix_offset[p] < 0) continue;
        t->primary[p].sym = (uint16_t)prefix_offset[p];
        t->primary[p].len = (uint16_t)((prefix_sub_bits[p] + pbits) | 0x8000);
    }
    return 0;
}

void huff_free_decode_table2(huff_decode_table_t *t) {
    free(t->secondary);
    t->secondary = NULL;
    t->secondary_size = 0;
    t->secondary_cap = 0;
}

/* ── Decode one symbol from bitstream using table ──────────── */

static inline int huff_decode(bit_reader_t *br,
                              const huff_entry_t *table, int table_bits) {
    uint32_t bits = br_peek(br, table_bits);
    huff_entry_t e = table[bits];
    br_read(br, e.len);  /* consume */
    return e.sym;
}

/* ── Tree serialization (DEFLATE 3-level encoding) ─────────── */

/*
 * RLE-encode a sequence of code lengths using the code-length alphabet:
 *   0-15  = literal code length
 *   16    = copy previous 3-6 times  (2 extra bits)
 *   17    = repeat 0 for 3-10 times  (3 extra bits)
 *   18    = repeat 0 for 11-138 times (7 extra bits)
 */
static int rle_encode(const uint8_t *lens, int n,
                      uint8_t *syms, uint8_t *extra, uint8_t *ebits) {
    int out = 0;
    for (int i = 0; i < n; ) {
        if (lens[i] == 0) {
            /* Count run of zeros */
            int run = 1;
            while (i + run < n && lens[i + run] == 0 && run < 138) run++;
            if (run >= 11) {
                syms[out] = 18; ebits[out] = 7; extra[out] = (uint8_t)(run - 11);
                out++; i += run;
            } else if (run >= 3) {
                syms[out] = 17; ebits[out] = 3; extra[out] = (uint8_t)(run - 3);
                out++; i += run;
            } else {
                for (int j = 0; j < run; j++) {
                    syms[out] = 0; ebits[out] = 0; extra[out] = 0; out++;
                }
                i += run;
            }
        } else {
            /* Emit the length itself */
            uint8_t val = lens[i];
            syms[out] = val; ebits[out] = 0; extra[out] = 0;
            out++; i++;
            /* Check for repeat of same value */
            int run = 0;
            while (i + run < n && lens[i + run] == val && run < 6) run++;
            if (run >= 3) {
                syms[out] = 16; ebits[out] = 2; extra[out] = (uint8_t)(run - 3);
                out++; i += run;
            }
        }
    }
    return out;
}

void huff_write_trees(bit_writer_t *bw,
                      const uint8_t *ll_lens, int n_ll,
                      const uint8_t *d_lens, int n_dist) {
    /* Trim trailing zeros (but keep at least 257 lit/len and 1 dist) */
    while (n_ll > 257 && ll_lens[n_ll - 1] == 0) n_ll--;
    while (n_dist > 1 && d_lens[n_dist - 1] == 0) n_dist--;

    /* Concatenate and RLE-encode */
    uint8_t combined[LITLEN_SYMS + DIST_SYMS];
    memcpy(combined, ll_lens, (size_t)n_ll);
    memcpy(combined + n_ll, d_lens, (size_t)n_dist);
    int total_lens = n_ll + n_dist;

    uint8_t rle_syms[LITLEN_SYMS + DIST_SYMS + 64];
    uint8_t rle_extra[LITLEN_SYMS + DIST_SYMS + 64];
    uint8_t rle_ebits[LITLEN_SYMS + DIST_SYMS + 64];
    int nrle = rle_encode(combined, total_lens, rle_syms, rle_extra, rle_ebits);

    /* Build Huffman tree for the RLE symbols (code-length alphabet) */
    uint32_t cl_freq[CODELEN_SYMS] = {0};
    for (int i = 0; i < nrle; i++) cl_freq[rle_syms[i]]++;

    uint8_t  cl_lens[CODELEN_SYMS];
    uint16_t cl_codes[CODELEN_SYMS];
    huff_build_lengths(cl_freq, CODELEN_SYMS, HUFF_CL_MAX_BITS, cl_lens);
    huff_build_codes(cl_lens, CODELEN_SYMS, cl_codes);

    /* Trim trailing zeros in permuted order */
    int hclen = CODELEN_SYMS;
    while (hclen > 4 && cl_lens[codelen_order[hclen - 1]] == 0) hclen--;

    /* Write header: HLIT(5), HDIST(5), HCLEN(4) */
    bw_write(bw, (uint32_t)(n_ll - 257), 5);
    bw_write(bw, (uint32_t)(n_dist - 1), 5);
    bw_write(bw, (uint32_t)(hclen - 4), 4);

    /* Write code-length code lengths (3 bits each, permuted order) */
    for (int i = 0; i < hclen; i++)
        bw_write(bw, cl_lens[codelen_order[i]], 3);

    /* Write RLE-encoded lit/len + distance lengths */
    for (int i = 0; i < nrle; i++) {
        int s = rle_syms[i];
        bw_write(bw, cl_codes[s], cl_lens[s]);
        if (rle_ebits[i] > 0)
            bw_write(bw, rle_extra[i], rle_ebits[i]);
    }
}

int huff_read_trees(bit_reader_t *br,
                     uint8_t *ll_lens, int *n_ll,
                     uint8_t *d_lens, int *n_dist) {
    int hlit  = (int)br_read(br, 5) + 257;
    int hdist = (int)br_read(br, 5) + 1;
    int hclen = (int)br_read(br, 4) + 4;
    // fix: dont actually trust hlit/hdist too much as it is user-controlled
    if (hlit > LITLEN_SYMS || hdist > DIST_SYMS) return -1;

    /* Read code-length code lengths */
    uint8_t cl_lens[CODELEN_SYMS];
    memset(cl_lens, 0, sizeof cl_lens);
    for (int i = 0; i < hclen; i++)
        cl_lens[codelen_order[i]] = (uint8_t)br_read(br, 3);

    /* Build decode table for code-length alphabet */
    huff_entry_t cl_table[1 << HUFF_CL_MAX_BITS];
    huff_build_decode_table(cl_lens, CODELEN_SYMS, cl_table, HUFF_CL_MAX_BITS);

    /* Decode the combined lit/len + distance code lengths */
    int total = hlit + hdist;
    uint8_t combined[LITLEN_SYMS + DIST_SYMS];
    memset(combined, 0, sizeof combined);

    int i = 0;
    while (i < total) {
        int sym = huff_decode(br, cl_table, HUFF_CL_MAX_BITS);
        if (sym < 16) {
            combined[i++] = (uint8_t)sym;
        } else if (sym == 16) {
            if (i == 0) return -1;
            int run = (int)br_read(br, 2) + 3;
            uint8_t prev = combined[i - 1];
            for (int j = 0; j < run && i < total; j++) combined[i++] = prev;
        } else if (sym == 17) {
            int run = (int)br_read(br, 3) + 3;
            for (int j = 0; j < run && i < total; j++) combined[i++] = 0;
        } else if (sym == 18) {
            int run = (int)br_read(br, 7) + 11;
            for (int j = 0; j < run && i < total; j++) combined[i++] = 0;
        } else {
            return -1;
        }
    }

    memcpy(ll_lens, combined, (size_t)hlit);
    memset(ll_lens + hlit, 0, (size_t)(LITLEN_SYMS - hlit));
    memcpy(d_lens, combined + hlit, (size_t)hdist);
    memset(d_lens + hdist, 0, (size_t)(DIST_SYMS - hdist));

    *n_ll = hlit;
    *n_dist = hdist;
    return 0;
}
