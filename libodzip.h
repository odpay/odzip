#ifndef LIBODZIP_H
#define LIBODZIP_H

#include <stdio.h>
#include <stdint.h>

#define ODZ_FORMAT_VERSION  2

/* Error codes */
#define ODZ_OK          0
#define ODZ_ERR_IO      1
#define ODZ_ERR_OOM     2
#define ODZ_ERR_FORMAT  3   /* bad magic, unsupported version */
#define ODZ_ERR_CORRUPT 4   /* data integrity error */

/* Progress callback.
 * Return 0 to continue, nonzero to abort. */
typedef int (*odz_progress_fn)(uint64_t processed, uint64_t total, void *userdata);

/* Options (pass NULL for defaults / no progress) */
typedef struct {
    odz_progress_fn progress;
    void *userdata;
    int threads;          /* 0 or 1 = single-threaded */
} odz_options_t;

int odz_compress(FILE *in, FILE *out, const odz_options_t *opts);
int odz_decompress(FILE *in, FILE *out, const odz_options_t *opts);
const char *odz_strerror(int err);

#endif
