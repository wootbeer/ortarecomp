/* CRI AIX: a movie's multichannel soundtrack, several ADX streams
 * interleaved in AIXP chunks (Orta's opening_bs.aix: three stereo streams
 * at 48 kHz, front, back and centre/LFE). Decoded whole into memory. */
#ifndef PDO_AIX_H
#define PDO_AIX_H

#include <stdbool.h>
#include <stdint.h>

#define PDO_AIX_MAX_STREAMS 8

typedef struct pdo_aix {
    int       streams;
    int       channels[PDO_AIX_MAX_STREAMS];
    uint32_t  rate[PDO_AIX_MAX_STREAMS];
    uint32_t  frames[PDO_AIX_MAX_STREAMS];      /* samples per channel */
    int16_t  *pcm[PDO_AIX_MAX_STREAMS];         /* interleaved */
} pdo_aix;

/* Reads and decodes an .aix file; false (and nothing to free) on failure. */
bool pdo_aix_load(const char *path, pdo_aix *out);
void pdo_aix_free(pdo_aix *a);

#endif
