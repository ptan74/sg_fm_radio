# app_radio_player

Plays a station's live MP3 stream through the ES8311 speaker using Espressif's
`esp_audio_simple_player` (GMF HTTP reader + MP3 decoder). Device only.

**Model.** Callers post *desired* state — station, paused, volume, online —
with non-blocking, latest-wins setters. A control task (`radio_ctl`) reconciles
desired against actual: it stops the running stream, starts the new one, or
applies the volume. Sweeping the dial therefore never queues a backlog of
stale tune requests. Pause stops the stream (resume rejoins the live
broadcast rather than playing buffered audio).

**Status.** `status_cb(state, station, ctx)` reports `IDLE`, `BUFFERING`,
`PLAYING` (after the decoder reports the stream format) or `ERROR`. It is
called from the control task and the decoder task; keep it non-blocking.

**Failures.** A stream that fails to open or ends unexpectedly is retried
with exponential backoff (2 s doubling to 30 s) while it stays selected,
unpaused and online. Choosing a different station resets the backoff.

**Persistence.** After a request settles, the station frequency and volume are
written to the default `nvs` partition, namespace `sg_radio`, only when they
changed. `app_radio_player_load_saved()` reads them at boot.

**Threading and ownership.** `want_*`, `online` and the event bits are atomics
written by any task. All other state belongs to `radio_ctl`. The decoder task
writes PCM to the codec and reopens it when the stream format is known; the
control task touches the codec only to set the volume. One player per boot;
there is no destroy (it lives until reboot).

**Resources.** Control task 4 KiB internal stack; decoder task 6 KiB stack in
PSRAM. Streams use plain HTTP: StreamTheWorld redirects `http://` requests to
HTTP edge servers, so no TLS certificate bundle is needed.

Dependencies: `app_stations`, `esp-mosaico-bsp` (speaker), `esp_audio_simple_player`,
`esp_codec_dev`, `nvs_flash`, FreeRTOS.

```c
app_radio_player_handle_t player;
app_radio_player_config_t cfg = {.initial_volume = 60, .status_cb = on_status};
ESP_ERROR_CHECK(app_radio_player_start(&cfg, &player));
app_radio_player_set_online(player, true);
app_radio_player_tune(player, 10);   /* Class 95 */
```
