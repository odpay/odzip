
#include <stdlib.h>
#include <string.h>

#include "odz.h"


void decompress_simple(const uint8_t *in, size_t n, uint8_t **outp, size_t *outn) {
	if (n < 8) die("truncated");
	if (in[0] != 'O' || in[1] != 'D' || in[2] != 'Z' || in[3] != ODZ_VERSION) die("bad magic");
	uint32_t raw_len = rd_u32le(in + 4);

	uint8_t *out = malloc(raw_len); if (!out) die("oom");
	size_t ip = 8, op = 0;

	while (ip < n && op < raw_len) {
		uint8_t flags = in[ip++]; // next 8 token types encoded in flags, 0 for match 1 for literal (ordered lsb->msb)

		for (int k = 0; k < 8 && op < raw_len; k++) {
			if (flags & (1u << k)) { // is a 3 byte match token
				if (ip + 3 > n) die("corrupt match token");
				int len = (int)in[ip++] + ODZ_MIN_MATCH; // length of match
				int dist = (int)in[ip++] | ((int)in[ip++] << 8); // LE encoded distance back from start of match token to copy from
				if (dist <= 0 || dist > (int)op) die("bad distance");
				size_t from = op - (size_t)dist;
				for (int t = 0; t < len; t++) {
					if (op >= raw_len) die("overrun");
					out[op++] = out[from + t];
				}
			} else { // literal token
				if (ip >= n) die("corrupt literal");
				out[op++] = in[ip++];
			}
		}
	}

	if (op != raw_len) die("size mismatch");
	*outp = out; *outn = op;
}