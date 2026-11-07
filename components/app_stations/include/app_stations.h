// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** FM broadcast band covered by the dial, in tenths of a MHz (87.5..108.0). */
#define APP_STATIONS_BAND_MIN_TENTHS 875U
#define APP_STATIONS_BAND_MAX_TENTHS 1080U

/** Sentinel returned by lookups that find no station. */
#define APP_STATIONS_NONE (-1)

/** Immutable description of one Singapore FM station. All strings are static. */
typedef struct {
    uint16_t freq_tenths;   /**< Broadcast frequency, tenths of a MHz. */
    const char *name;       /**< Display name, ASCII. */
    const char *tagline;    /**< Format and owner, ASCII. */
    const char *stream_url; /**< Official live MP3 stream (may redirect). */
} app_station_t;

/** Number of stations; the table is sorted by ascending frequency. */
size_t app_stations_count(void);

/** Station at index, or NULL when index is out of range. */
const app_station_t *app_stations_get(size_t index);

/** Index of the station within tolerance_tenths of freq_tenths (closest wins),
 * or APP_STATIONS_NONE. */
int app_stations_find(uint16_t freq_tenths, uint16_t tolerance_tenths);

/** Index of the station closest to freq_tenths; never NONE for a non-empty table. */
int app_stations_nearest(uint16_t freq_tenths);

/** Next station strictly above (direction > 0) or below (direction < 0)
 * freq_tenths, wrapping around the band. */
int app_stations_seek(uint16_t freq_tenths, int direction);

#ifdef __cplusplus
}
#endif
