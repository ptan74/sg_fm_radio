# app_stations

Immutable table of Singapore FM stations: frequency, display name, tagline
and official live MP3 stream URL. Portable C with no ESP-IDF dependencies, so
the device player and the native simulator backend share it.

- Frequencies are in tenths of a MHz (`950` = 95.0 MHz) inside
  `APP_STATIONS_BAND_MIN_TENTHS..APP_STATIONS_BAND_MAX_TENTHS`.
- The table is sorted by frequency; lookups are linear (17 entries).
- All returned pointers reference static storage and are valid forever.
  Functions are pure and safe to call from any task.

Stream URLs use StreamTheWorld's `livestream-redirect` endpoint, which answers
with an HTTP redirect to the current edge server. They were checked on
2026-11-07; broadcasters can rename callsigns, so re-check them with `curl -IL`
when a station stops playing.

`tools/gen_dial_strip.py` reads the `.freq_tenths` values from
`app_stations.c` to place the station markers on the dial image. Regenerate
the image after changing the table.

```c
int idx = app_stations_find(freq_tenths, 1);   /* within ±0.1 MHz */
if (idx != APP_STATIONS_NONE) {
    const app_station_t *s = app_stations_get((size_t)idx);
}
```
