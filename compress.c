#include <stdlib.h>
#include <string.h>

#include "odz.h"
#include "lz_matcher.h"

void compress_simple(const uint8_t *in, size_t n, uint8_t **outp, size_t *outn) {
    size_t cap = n + n/7 + 32;
    uint8_t *out = malloc(cap); if (!out) die("oom");
    size_t op = 0;

    const char magic[4] = {'O','D','Z', ODZ_VERSION};
    memcpy(out + op, magic, 4); op += 4;
    uint8_t sz[4]; wr_u32le(sz, (uint32_t)n); memcpy(out+op, sz, 4); op += 4;

    // init matcher per block (here: whole buffer = one block)
    lz_matcher_t m;
    if (lz_matcher_init(&m, n, HASH_BITS, MAX_CHAIN_STEPS) != 0) die("oom");

    size_t i = 0;
    while (i < n) {
        if (op + 1 + 8*3 + 16 >= cap) { cap = cap*2 + 1024; out = realloc(out, cap); if (!out) die("oom"); }

        size_t flag_pos = op++;
        uint8_t flags = 0;

        for (int k = 0; k < 8 && i < n; k++) {
            int best_len = 0, best_dist = 0;

            // IMPORTANT: ensure position is in chains before search at i
            // If you want exact parity with the naïve encoder,
            // insert AFTER deciding. A common choice is: insert current i before searching i,
            // and when you take a match of length L, optionally insert i+1..i+min(L-1, 2) as well.
            lz_matcher_find_best(&m, in, i, n, (int)ODZ_WINDOW, ODZ_MIN_MATCH, ODZ_MAX_MATCH, &best_len, &best_dist);

            // --- lazy matching (optional, cheap) ---
            if (best_len == ODZ_MIN_MATCH && i + 1 < n) {
                // insert i before peeking next, so next search sees this position too
                lz_matcher_insert(&m, in, i);
                int next_len=0, next_dist=0;
                lz_matcher_find_best_next(&m, in, i, n, (int)ODZ_WINDOW, ODZ_MIN_MATCH, ODZ_MAX_MATCH, &next_len, &next_dist);
                if (next_len > best_len) { // prefer longer future match
                    out[op++] = in[i++];
                    continue; // token k done (literal), skip to next k
                }
            }

            if (best_len >= ODZ_MIN_MATCH) {
                flags |= (1u << k);
                out[op++] = (uint8_t)(best_len - ODZ_MIN_MATCH);
                out[op++] = (uint8_t)(best_dist & 0xFF);
                out[op++] = (uint8_t)(best_dist >> 8);

                // insert positions covered by the match (improves future hits)
                // common heuristic: insert first 1–2 following positions
                size_t end = i + (size_t)best_len;
                lz_matcher_insert(&m, in, i);
                if (i + 1 < end) lz_matcher_insert(&m, in, i + 1);
                if (i + 2 < end) lz_matcher_insert(&m, in, i + 2);

                i += (size_t)best_len;
            } else {
                // no match → literal; insert this position then emit
                lz_matcher_insert(&m, in, i);
                out[op++] = in[i++];
            }
        }

        out[flag_pos] = flags;
    }

    lz_matcher_free(&m);
    *outp = out; *outn = op;
}
