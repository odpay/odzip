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
#include <stdlib.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "libodzip.h"

static void die(const char *m) { fprintf(stderr, "odz: error: %s\n", m); exit(1); }

static int ncpus(void) {
#ifdef _WIN32
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (int)si.dwNumberOfProcessors;
#else
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
#endif
}

static int verbosity = 1;

static int progress_cb(uint64_t processed, uint64_t total, void *userdata) {
    (void)userdata;
    fprintf(stderr, "\r  %llu / %llu bytes  (%.1f%%)",
            (unsigned long long)processed,
            (unsigned long long)total,
            total > 0 ? 100.0 * processed / total : 100.0);
    return 0;
}

static int file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static const char *base_name(const char *path) {
    const char *p = strrchr(path, '/');
    return p ? p + 1 : path;
}

static int ends_with_odz(const char *s) {
    size_t len = strlen(s);
    return len >= 4 && strcmp(s + len - 4, ".odz") == 0;
}

static void usage(const char *prog) {
    fprintf(stderr,
        "odz — LZ77+Huffman compressor (format v%d)\n\n"
        "usage:\n"
        "  %s [options] <input>\n"
        "  %s [options] <input> <output>\n"
        "  %s [options] c <input> <output>\n"
        "  %s [options] d <input> <output>\n\n"
        "options:\n"
        "  -c              force compress\n"
        "  -d              force decompress\n"
        "  -o, --out FILE  output file\n"
        "  -f, --force     overwrite existing output\n"
        "  -j N            use N threads (default: all cores)\n"
        "  -v0             silent\n"
        "  -v1             progress (default)\n"
        "  -v2             verbose (progress + summary)\n"
        "  -h, --help      show this help\n\n"
        "Auto-detects mode from extension:\n"
        "  file.txt     → compress  → file.txt.odz\n"
        "  file.txt.odz → decompress → file.txt\n",
        ODZ_FORMAT_VERSION, prog, prog, prog, prog);
}

int main(int argc, char **argv) {
    int force = 0;
    int mode = 0;   /* 0=auto, 'c'=compress, 'd'=decompress */
    int threads = 0;
    const char *out_path = NULL;
    const char *positionals[3];
    int npos = 0;

    for (int i = 1; i < argc; i++) {
        char *a = argv[i];
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            usage(argv[0]); return 0;
        } else if (strcmp(a, "-f") == 0 || strcmp(a, "--force") == 0) {
            force = 1;
        } else if (strcmp(a, "-c") == 0) {
            mode = 'c';
        } else if (strcmp(a, "-d") == 0) {
            mode = 'd';
        } else if (strcmp(a, "-v0") == 0) {
            verbosity = 0;
        } else if (strcmp(a, "-v1") == 0) {
            verbosity = 1;
        } else if (strcmp(a, "-v2") == 0) {
            verbosity = 2;
        } else if (strcmp(a, "-j") == 0) {
            if (++i >= argc) die("missing argument for -j");
            threads = atoi(argv[i]);
            if (threads < 1 || threads > 256) die("thread count must be 1-256");
        } else if (strcmp(a, "-o") == 0 || strcmp(a, "--out") == 0) {
            if (++i >= argc) die("missing argument for -o");
            out_path = argv[i];
        } else if (a[0] == '-' && a[1] != '\0') {
            fprintf(stderr, "odz: unknown option: %s\n", a);
            usage(argv[0]); return 2;
        } else {
            if (npos >= 3) { usage(argv[0]); return 2; }
            positionals[npos++] = a;
        }
    }

    /* Parse positional arguments */
    const char *in_path = NULL;

    /* Legacy: "c <in> <out>" / "d <in> <out>" */
    if (npos >= 1 && strlen(positionals[0]) == 1 &&
        (positionals[0][0] == 'c' || positionals[0][0] == 'd')) {
        mode = positionals[0][0];
        if (npos >= 2) in_path = positionals[1];
        if (npos >= 3 && !out_path) out_path = positionals[2];
    } else {
        if (npos >= 1) in_path = positionals[0];
        if (npos >= 2 && !out_path) out_path = positionals[1];
    }

    if (!in_path) { usage(argv[0]); return 2; }

    /* Auto-detect mode from extension */
    if (mode == 0)
        mode = ends_with_odz(in_path) ? 'd' : 'c';

    /* Auto-generate output path in current directory */
    char auto_out[4096];
    if (!out_path) {
        const char *base = base_name(in_path);
        if (mode == 'c') {
            snprintf(auto_out, sizeof(auto_out), "%s.odz", base);
        } else {
            if (ends_with_odz(base)) {
                size_t len = strlen(base) - 4;
                memcpy(auto_out, base, len);
                auto_out[len] = '\0';
            } else {
                snprintf(auto_out, sizeof(auto_out), "%s.raw", base);
            }
        }
        out_path = auto_out;
    }

    /* Refuse to overwrite without --force */
    if (!force && file_exists(out_path)) {
        fprintf(stderr, "odz: '%s' already exists (use -f to overwrite)\n", out_path);
        return 1;
    }

    FILE *fin = fopen(in_path, "rb");
    if (!fin) die("cannot open input file");

    FILE *fout = fopen(out_path, "wb");
    if (!fout) { fclose(fin); die("cannot open output file"); }

    if (threads == 0) threads = ncpus();

    odz_options_t opts = {
        .progress = (verbosity >= 1) ? progress_cb : NULL,
        .userdata = NULL,
        .threads = threads
    };

    if (verbosity >= 2)
        fprintf(stderr, "%s %s → %s\n",
                mode == 'c' ? "compress" : "decompress", in_path, out_path);

    int rc;
    if (mode == 'c')
        rc = odz_compress(fin, fout, &opts);
    else
        rc = odz_decompress(fin, fout, &opts);

    if (verbosity >= 1)
        fprintf(stderr, "\n");

    if (rc != ODZ_OK) {
        fclose(fin); fclose(fout);
        remove(out_path);
        die(odz_strerror(rc));
    }

    /* Verbose summary */
    if (verbosity >= 2) {
        fseek(fin, 0, SEEK_END);
        long in_size = ftell(fin);
        fseek(fout, 0, SEEK_END);
        long out_size = ftell(fout);
        if (mode == 'c')
            fprintf(stderr, "  %ld → %ld bytes (%.1f%%)\n",
                    in_size, out_size,
                    in_size > 0 ? 100.0 * out_size / in_size : 0.0);
        else
            fprintf(stderr, "  %ld → %ld bytes\n", in_size, out_size);
    }

    fclose(fin);
    fclose(fout);
    return 0;
}
