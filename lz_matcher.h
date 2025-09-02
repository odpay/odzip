#ifndef LZ_MATCHER_H
#define LZ_MATCHER_H
#include <stdint.h>
#include <stddef.h>

typedef struct {
	int32_t *head;
	int32_t *prev;
	size_t   n;
	uint32_t hash_mask;
	int      max_chain_steps;
} lz_matcher_t;

#define HASH_BITS 15
#define MAX_CHAIN_STEPS 64

int  lz_matcher_init(lz_matcher_t *m, size_t n_block, int hash_bits, int max_chain_steps);
void lz_matcher_reset(lz_matcher_t *m, size_t n_block);
void lz_matcher_free(lz_matcher_t *m);

/* NOTE: plain prototype (no static/inline) */
void lz_matcher_insert(lz_matcher_t *m, const uint8_t *in, size_t i);

void lz_matcher_find_best(const lz_matcher_t *m, const uint8_t *in, size_t i, size_t n,
						  int window, int min_match, int max_match,
						  int *out_len, int *out_dist);

void lz_matcher_find_best_next(const lz_matcher_t *m, const uint8_t *in, size_t i, size_t n,
							   int window, int min_match, int max_match,
							   int *out_len, int *out_dist);
#endif
