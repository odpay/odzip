#include "lz_matcher.h"
#include <stdlib.h>
#include <string.h>

static uint32_t hash3(uint8_t a, uint8_t b, uint8_t c, uint32_t mask){
    uint32_t k = ((uint32_t)a<<16) ^ ((uint32_t)b<<8) ^ (uint32_t)c;
    return (k * 2654435761u) & mask; // mask = (1<<hash_bits)-1
}

int lz_matcher_init(lz_matcher_t *m, size_t n_block, int hash_bits, int max_chain_steps){
    size_t hash_size = (size_t)1 << hash_bits;
    m->head = (int32_t*)malloc(hash_size * sizeof *m->head);
    m->prev = (int32_t*)malloc(n_block   * sizeof *m->prev);
    if (!m->head || !m->prev) { free(m->head); free(m->prev); return -1; }
    m->n = n_block;
    m->hash_mask = (uint32_t)hash_size - 1u;
    m->max_chain_steps = max_chain_steps;
    memset(m->head, 0xFF, hash_size * sizeof *m->head); // -1
    return 0;
}

void lz_matcher_reset(lz_matcher_t *m, size_t n_block){
    m->n = n_block;
    // keep capacity; caller must ensure prev[] big enough or recreate
    memset(m->head, 0xFF, ((size_t)m->hash_mask + 1) * sizeof *m->head);
}

void lz_matcher_free(lz_matcher_t *m){
    free(m->head); free(m->prev);
    m->head = m->prev = NULL; m->n = 0;
}

void lz_matcher_insert(lz_matcher_t *m, const uint8_t *in, size_t i) {
    if (i + 2 >= m->n) { m->prev[i] = -1; return; }
    uint32_t h = hash3(in[i], in[i+1], in[i+2], m->hash_mask);
    m->prev[i] = m->head[h];
    m->head[h] = (int32_t)i;
}

static int match_len(const uint8_t *a, const uint8_t *b, int maxl){
    int l = 0;
    // word-wise then tail
    while (l + (int)sizeof(size_t) <= maxl &&
           *(const size_t*)(a + l) == *(const size_t*)(b + l)) l += (int)sizeof(size_t);
    while (l < maxl && a[l] == b[l]) l++;
    return l;
}

void lz_matcher_find_best(const lz_matcher_t *m, const uint8_t *in, size_t i, size_t n,
                          int window, int min_match, int max_match,
                          int *out_len, int *out_dist)
{
    int best_len = 0, best_dist = 0;
    if (i + (size_t)min_match <= n) {
        uint32_t h = hash3(in[i], in[i+1], in[i+2], m->hash_mask);
        int32_t p = m->head[h];
        int steps = 0;
        int maxl = (int)((n - i) < (size_t)max_match ? (n - i) : (size_t)max_match);

        while (p >= 0 && steps++ < m->max_chain_steps) {
            int dist = (int)(i - (size_t)p);
            if (dist > 0 && dist <= window) {
                int l = match_len(in + p, in + i, maxl);
                if (l >= min_match && (l > best_len || (l == best_len && dist < best_dist))) {
                    best_len = l; best_dist = dist;
                    if (l == maxl) break; // best possible at this i
                }
            }
            p = m->prev[p];
        }
    }
    *out_len = best_len; *out_dist = best_dist;
}

void lz_matcher_find_best_next(const lz_matcher_t *m, const uint8_t *in, size_t i, size_t n,
                               int window, int min_match, int max_match,
                               int *out_len, int *out_dist)
{
    if (i + 1 >= n) { *out_len = 0; *out_dist = 0; return; }
    // pretend i+1 is the current position; safe because matcher chains are built incrementally (see compressor loop)
    lz_matcher_find_best(m, in, i+1, n, window, min_match, max_match, out_len, out_dist);
}
