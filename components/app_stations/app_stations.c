// SPDX-License-Identifier: Apache-2.0
#include "app_stations.h"

#include <stdlib.h>

#define STREAM(callsign) \
    "http://playerservices.streamtheworld.com/api/livestream-redirect/" callsign ".mp3"

/* Sorted by frequency. tools/gen_dial_strip.py parses the freq_tenths values
 * to draw station markers, so keep one `.freq_tenths = N` per entry. */
static const app_station_t s_stations[] = {
    {.freq_tenths = 883,  .name = "88.3 JIA FM",   .tagline = "Mandarin hits - SPH",          .stream_url = STREAM("883JIA")},
    {.freq_tenths = 893,  .name = "Money FM 89.3", .tagline = "Business news - SPH",          .stream_url = STREAM("MONEY_893")},
    {.freq_tenths = 897,  .name = "Ria 89.7",      .tagline = "Malay pop - Mediacorp",        .stream_url = STREAM("RIA897FM")},
    {.freq_tenths = 905,  .name = "Gold 905",      .tagline = "English classics - Mediacorp", .stream_url = STREAM("GOLD905")},
    {.freq_tenths = 913,  .name = "ONE FM 91.3",   .tagline = "English talk - SPH",           .stream_url = STREAM("ONE_FM_913")},
    {.freq_tenths = 920,  .name = "Kiss92",        .tagline = "English adult pop - SPH",      .stream_url = STREAM("KISS_92")},
    {.freq_tenths = 924,  .name = "Symphony 924",  .tagline = "Classical - Mediacorp",        .stream_url = STREAM("SYMPHONY924")},
    {.freq_tenths = 933,  .name = "YES 933",       .tagline = "Mandarin pop - Mediacorp",     .stream_url = STREAM("YES933")},
    {.freq_tenths = 938,  .name = "CNA938",        .tagline = "News and talk - Mediacorp",    .stream_url = STREAM("938NOW")},
    {.freq_tenths = 942,  .name = "Warna 94.2",    .tagline = "Malay - Mediacorp",            .stream_url = STREAM("WARNA942FM")},
    {.freq_tenths = 950,  .name = "Class 95",      .tagline = "English pop - Mediacorp",      .stream_url = STREAM("CLASS95")},
    {.freq_tenths = 958,  .name = "Capital 95.8",  .tagline = "Mandarin news - Mediacorp",    .stream_url = STREAM("CAPITAL958FM")},
    {.freq_tenths = 963,  .name = "Hao FM 96.3",   .tagline = "Mandarin classics - SPH",      .stream_url = STREAM("HAO_963")},
    {.freq_tenths = 968,  .name = "Oli 96.8",      .tagline = "Tamil - Mediacorp",            .stream_url = STREAM("OLI968FM")},
    {.freq_tenths = 972,  .name = "Love 972",      .tagline = "Mandarin ballads - Mediacorp", .stream_url = STREAM("LOVE972FM")},
    {.freq_tenths = 987,  .name = "987",           .tagline = "Youth hits - Mediacorp",       .stream_url = STREAM("987FM")},
    {.freq_tenths = 1003, .name = "UFM 100.3",     .tagline = "Mandarin - SPH",               .stream_url = STREAM("UFM_1003")},
};

#define STATION_COUNT (sizeof(s_stations) / sizeof(s_stations[0]))

size_t app_stations_count(void)
{
    return STATION_COUNT;
}

const app_station_t *app_stations_get(size_t index)
{
    return index < STATION_COUNT ? &s_stations[index] : NULL;
}

int app_stations_nearest(uint16_t freq_tenths)
{
    int best = APP_STATIONS_NONE;
    unsigned best_distance = UINT16_MAX;
    for (size_t i = 0; i < STATION_COUNT; ++i) {
        const unsigned distance = (unsigned)abs((int)s_stations[i].freq_tenths - (int)freq_tenths);
        if (distance < best_distance) {
            best_distance = distance;
            best = (int)i;
        }
    }
    return best;
}

int app_stations_find(uint16_t freq_tenths, uint16_t tolerance_tenths)
{
    const int nearest = app_stations_nearest(freq_tenths);
    if (nearest == APP_STATIONS_NONE) {
        return APP_STATIONS_NONE;
    }
    const int distance = abs((int)s_stations[nearest].freq_tenths - (int)freq_tenths);
    return distance <= (int)tolerance_tenths ? nearest : APP_STATIONS_NONE;
}

int app_stations_seek(uint16_t freq_tenths, int direction)
{
    if (STATION_COUNT == 0 || direction == 0) {
        return APP_STATIONS_NONE;
    }
    if (direction > 0) {
        for (size_t i = 0; i < STATION_COUNT; ++i) {
            if (s_stations[i].freq_tenths > freq_tenths) {
                return (int)i;
            }
        }
        return 0;
    }
    for (size_t i = STATION_COUNT; i-- > 0;) {
        if (s_stations[i].freq_tenths < freq_tenths) {
            return (int)i;
        }
    }
    return (int)STATION_COUNT - 1;
}
