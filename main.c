// main.c
// Format: "ODZ\VERSION" | raw_size(u32 LE) | [ groups ... ]
// Each group: flags(u8), then up to 8 tokens (LSB-first):
//   bit=0 → literal: [u8]
//   bit=1 → match:   [len_minus_MIN_MATCH u8][dist u16 LE], with len in [MIN_MATCH..MAX_MATCH]
//
// Build: gcc -std=c17 -O2 -Wall -Wextra -o odz main.c compress.c decompress.c lz_hashchain.c odz_util.c

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <assert.h>

#include "odz.h"


enum { HASH_BITS = 15, HASH_SIZE = 1 << HASH_BITS };


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
