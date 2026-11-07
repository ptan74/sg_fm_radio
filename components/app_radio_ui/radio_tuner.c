// SPDX-License-Identifier: Apache-2.0
#include "radio_tuner.h"

#include "app_stations.h"

static uint16_t clamp_freq(int32_t tenths)
{
    if (tenths < (int32_t)APP_STATIONS_BAND_MIN_TENTHS) {
        return APP_STATIONS_BAND_MIN_TENTHS;
    }
    if (tenths > (int32_t)APP_STATIONS_BAND_MAX_TENTHS) {
        return APP_STATIONS_BAND_MAX_TENTHS;
    }
    return (uint16_t)tenths;
}

static uint8_t clamp_volume(uint8_t volume)
{
    unsigned v = volume;
    if (v < RADIO_TUNER_VOLUME_MIN) {
        v = RADIO_TUNER_VOLUME_MIN;
    } else if (v > RADIO_TUNER_VOLUME_MAX) {
        v = RADIO_TUNER_VOLUME_MAX;
    }
    /* Round to the nearest step so the cycle stays on 20/40/60/80/100. */
    v = ((v + RADIO_TUNER_VOLUME_STEP / 2) / RADIO_TUNER_VOLUME_STEP) * RADIO_TUNER_VOLUME_STEP;
    return (uint8_t)v;
}

void radio_tuner_init(radio_tuner_t *t, uint16_t freq_tenths, uint8_t volume)
{
    *t = (radio_tuner_t) {
        .committed = APP_STATIONS_NONE,
        .volume = clamp_volume(volume),
    };
    radio_tuner_set_freq(t, freq_tenths);
}

void radio_tuner_set_freq(radio_tuner_t *t, uint16_t freq_tenths)
{
    t->freq_tenths = clamp_freq(freq_tenths);
    t->locked = app_stations_find(t->freq_tenths, 0);
}

void radio_tuner_select(radio_tuner_t *t, int station)
{
    const app_station_t *s = station < 0 ? NULL : app_stations_get((size_t)station);
    if (s) {
        radio_tuner_set_freq(t, s->freq_tenths);
    }
}

void radio_tuner_seek(radio_tuner_t *t, int direction)
{
    radio_tuner_select(t, app_stations_seek(t->freq_tenths, direction));
}

void radio_tuner_jog(radio_tuner_t *t, int32_t jog)
{
    if (!t->dragging) {
        t->dragging = true;
        t->jog_origin = jog;
        t->drag_f0 = t->freq_tenths;
        return;
    }
    radio_tuner_set_freq(t, clamp_freq((int32_t)t->drag_f0 - (jog - t->jog_origin)));
}

void radio_tuner_drag_end(radio_tuner_t *t)
{
    if (!t->dragging) {
        return;
    }
    t->dragging = false;
    radio_tuner_select(t, app_stations_find(t->freq_tenths, RADIO_TUNER_SNAP_TENTHS));
}

bool radio_tuner_commit(radio_tuner_t *t)
{
    if (t->committed == t->locked) {
        return false;
    }
    t->committed = t->locked;
    return true;
}

uint8_t radio_tuner_cycle_volume(radio_tuner_t *t)
{
    const unsigned next = t->volume + RADIO_TUNER_VOLUME_STEP;
    t->volume = (uint8_t)(next > RADIO_TUNER_VOLUME_MAX ? RADIO_TUNER_VOLUME_MIN : next);
    return t->volume;
}

int32_t radio_tuner_dial_x(uint16_t freq_tenths)
{
    return -((int32_t)clamp_freq(freq_tenths) - (int32_t)APP_STATIONS_BAND_MIN_TENTHS) *
           RADIO_TUNER_PX_PER_TENTH;
}
