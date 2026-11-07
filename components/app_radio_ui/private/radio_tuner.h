// SPDX-License-Identifier: Apache-2.0
#pragma once

/* Pure tuning state machine: frequency, drag-to-sweep, station lock, seek and
 * the "committed" station the audio sink should play. No GSP or RTOS calls, so
 * it behaves identically on device and in the simulator. Single owner: the
 * app_radio_ui controller, on the GSP render task. */

#include <stdbool.h>
#include <stdint.h>

/** Pixels the dial strip moves per 0.1 MHz; must match tools/gen_dial_strip.py
 * (PX_PER_MHZ / 10). */
#define RADIO_TUNER_PX_PER_TENTH 6

/* Dragging uses a relative "jog" position (the invisible dial slider, 0..100
 * across the screen width): one jog step moves the dial by one tenth. */

/** Release snaps to a station within this distance (tenths of a MHz). */
#define RADIO_TUNER_SNAP_TENTHS 1

#define RADIO_TUNER_VOLUME_STEP 20U
#define RADIO_TUNER_VOLUME_MIN 20U
#define RADIO_TUNER_VOLUME_MAX 100U

typedef struct {
    uint16_t freq_tenths; /**< Current dial position, band-clamped. */
    int locked;           /**< Station exactly under the needle, or APP_STATIONS_NONE. */
    int committed;        /**< Station last handed to the audio sink, or NONE. */
    bool paused;
    uint8_t volume;       /**< Percent, RADIO_TUNER_VOLUME_MIN..MAX. */
    bool dragging;
    int32_t jog_origin;
    uint16_t drag_f0;
} radio_tuner_t;

/** Start at freq_tenths (clamped) with volume (rounded to a valid step). */
void radio_tuner_init(radio_tuner_t *t, uint16_t freq_tenths, uint8_t volume);

/** Move the dial to an absolute frequency (clamped) and refresh the lock. */
void radio_tuner_set_freq(radio_tuner_t *t, uint16_t freq_tenths);

/** Jump to a station index; ignored when out of range. */
void radio_tuner_select(radio_tuner_t *t, int station);

/** Seek to the next station above (direction > 0) or below, wrapping. */
void radio_tuner_seek(radio_tuner_t *t, int direction);

/** Feed a jog position. The first sample of a gesture sets the origin; later
 * samples move the dial by (origin - jog) tenths, so dragging right lowers the
 * frequency like pulling a physical scale under a fixed needle. */
void radio_tuner_jog(radio_tuner_t *t, int32_t jog);
/** Ends the gesture and snaps to a station within RADIO_TUNER_SNAP_TENTHS. */
void radio_tuner_drag_end(radio_tuner_t *t);

/** Make the locked station (or silence) the committed one. Returns true when
 * the committed station changed and the sink must be told. */
bool radio_tuner_commit(radio_tuner_t *t);

/** Advance volume one step, wrapping from MAX back to MIN; returns it. */
uint8_t radio_tuner_cycle_volume(radio_tuner_t *t);

/** Dial strip x offset that puts freq_tenths under the centre needle. */
int32_t radio_tuner_dial_x(uint16_t freq_tenths);
