/*
 * odz — a DEFLATE-class compressor
 *
 * Format v2: "ODZ\x02" | original_size(u64 LE) | blocks...
 * Each block: flags(u8) | raw_size(u32 LE) | [compressed_size(u32 LE)] | data
 *
 * Compression pipeline: LZ77 hash-chain → Huffman → bitstream
 * Processes input in 1 MB blocks for bounded memory usage.
 *
 * Build: cmake --build . --config Release
 */

#include <stdio.h>
#include <string.h>

#include "odz.h"

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr,
            "odz — LZ77+Huffman compressor (format v%d)\n"
            "usage:\n"
            "  %s c <input> <output>   compress\n"
            "  %s d <input> <output>   decompress\n",
            ODZ_VERSION, argv[0], argv[0]);
        return 2;
    }

    char mode = argv[1][0];

    FILE *fin = fopen(argv[2], "rb");
    if (!fin) die("cannot open input file");

    FILE *fout = fopen(argv[3], "wb");
    if (!fout) { fclose(fin); die("cannot open output file"); }

    if (mode == 'c') {
        odz_compress(fin, fout);
    } else if (mode == 'd') {
        odz_decompress(fin, fout);
    } else {
        die("mode must be 'c' or 'd'");
    }

    fclose(fin);
    fclose(fout);
    return 0;
}
