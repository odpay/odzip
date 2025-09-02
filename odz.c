// odz.c
// Format: "ODZ\VERSION" | raw_size(u32 LE) | [ groups ... ]
// Each group: flags(u8), then up to 8 tokens (LSB-first):
//   bit=0 → literal: [u8]
//   bit=1 → match:   [len_minus_MIN_MATCH u8][dist u16 LE], with len in [MIN_MATCH..MAX_MATCH]
//
// Build: gcc -std=c17 -O2 -Wall -Wextra -o odz odz.c

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define VERSION 1
enum { WINDOW = 4096, MIN_MATCH = 3, MAX_MATCH = 18 };

static void die(const char *m) { fprintf(stderr, "err: %s\n", m); exit(1); }

static uint32_t rd_u32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}
static void wr_u32le(uint8_t *p, uint32_t v) {
    p[0]=v&0xFF; p[1]=(v>>8)&0xFF; p[2]=(v>>16)&0xFF; p[3]=(v>>24)&0xFF;
}

static uint8_t *read_entire(const char *path, size_t *n_out) {
    FILE *f = fopen(path, "rb"); if (!f) die("open input");
    if (fseek(f, 0, SEEK_END)) die("seek end");
    long n = ftell(f); if (n < 0) die("ftell");
    if (fseek(f, 0, SEEK_SET)) die("seek set");
    uint8_t *buf = malloc((size_t)n); if (!buf) die("oom");
    size_t got = fread(buf, 1, (size_t)n, f);
    if (got != (size_t)n) die("short read");
    fclose(f);
    *n_out = got; return buf;
}
static void write_all(const char *path, const uint8_t *buf, size_t n) {
    FILE *f = fopen(path, "wb"); if (!f) die("open output");
    if (fwrite(buf, 1, n, f) != n) die("short write");
    fclose(f);
}


static void compress_simple(const uint8_t *in, size_t n, uint8_t **outp, size_t *outn) {
    // very generous capacity guess: literals worst-case → 1 flag per 8 + 1 byte each ≈ n * 1.125 + header
    size_t cap = n + n/7 + 32;
    uint8_t *out = malloc(cap); if (!out) die("oom");
    size_t op = 0;

    // header: "ODZ", version, raw_size
    const char magic[4] = {'O','D','Z',VERSION};
    memcpy(out + op, magic, 4); op += 4;
    uint8_t sz[4]; wr_u32le(sz, (uint32_t)n); memcpy(out+op, sz, 4); op += 4;

    size_t i = 0;
    while (i < n) {
        // reserve space (expand if needed)
        if (op + 1 + 8*3 + 16 >= cap) { cap = cap*2 + 1024; out = realloc(out, cap); if (!out) die("oom"); }

        size_t flag_pos = op++;
        uint8_t flags = 0;
        // int tokens = 0;

        for (int k = 0; k < 8 && i < n; k++) {
            // naïve best-match search in previous WINDOW bytes, length up to MAX_MATCH (bounded)
            int best_len = 0, best_dist = 0;

            if (i + MIN_MATCH <= n) {
                size_t start = (i > (size_t)WINDOW ? i - WINDOW : 0);
                size_t maxl = (n - i < (size_t)MAX_MATCH) ? (int)(n - i) : MAX_MATCH;
                for (size_t p = i; p-- > start; ) {
                    int dist = (int)(i - p);
                    int l = 0;
                    while (l < maxl && in[p + l] == in[i + l]) l++;
                    if (l >= MIN_MATCH && l > best_len) { best_len = l; best_dist = dist; if (l == MAX_MATCH) break; }
                }
            }

            if (best_len >= MIN_MATCH) {
                flags |= (1u << k);
                out[op++] = (uint8_t)(best_len - MIN_MATCH);
                out[op++] = (uint8_t)(best_dist & 0xFF);
                out[op++] = (uint8_t)(best_dist >> 8);
                i += (size_t)best_len;
            } else {
                out[op++] = in[i++];
            }
            // tokens++;
        }

        out[flag_pos] = flags;
    }

    *outp = out; *outn = op;
}

static void decompress_simple(const uint8_t *in, size_t n, uint8_t **outp, size_t *outn) {
    if (n < 8) die("truncated");
    if (in[0] != 'O' || in[1] != 'D' || in[2] != 'Z' || in[3] != VERSION) die("bad magic");
    uint32_t raw_len = rd_u32le(in + 4);

    uint8_t *out = malloc(raw_len); if (!out) die("oom");
    size_t ip = 8, op = 0;

    while (ip < n && op < raw_len) {
        uint8_t flags = in[ip++]; // next 8 token types encoded in flags, 0 for match 1 for literal (ordered lsb->msb)

        for (int k = 0; k < 8 && op < raw_len; k++) {
            if (flags & (1u << k)) { // is a 3 byte match token
                if (ip + 3 > n) die("corrupt match token");
                int len = (int)in[ip++] + MIN_MATCH; // length of match
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

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "usage:\n  %s c <in> <out>\n  %s d <in> <out>\n", argv[0], argv[0]);
        return 2;
    }
    char mode = argv[1][0];
    size_t nin; uint8_t *bin = read_entire(argv[2], &nin);
    uint8_t *bout = NULL; size_t nout = 0;

    if (mode == 'c') {
        compress_simple(bin, nin, &bout, &nout);
    } else if (mode == 'd') {
        decompress_simple(bin, nin, &bout, &nout);
    } else {
        die("mode must be c or d");
    }

    write_all(argv[3], bout, nout);
    free(bin); free(bout);
    return 0;
}
