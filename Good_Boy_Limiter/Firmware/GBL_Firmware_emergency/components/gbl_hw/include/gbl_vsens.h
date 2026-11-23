/*
 * GBL - PDB voltage sensing
 *
 * Reads the output voltage of each PDB (Power Distribution Board) through the
 * Vsens1..4 inputs and summarises the connected ones (min / max).
 */
#pragma once

#include <stdbool.h>

/* Number of sensing inputs on the GBL board. Part of the API because it sizes
 * gbl_vsens_t; all tuning parameters live in gbl_vsens.c. */
#define GBL_VSENS_CHANNELS 4

typedef struct
{
	float volt[GBL_VSENS_CHANNELS]; // per-channel voltage [V], -1 on read failure
	float min;						// lowest voltage among the enabled channels
	float max;						// highest voltage among the enabled channels
	bool valid;						// false if no channel is enabled
} gbl_vsens_t;

/* Configures ADC units, channels and calibration. Call once at start-up. */
void gbl_vsens_init(void);

/* Samples every channel (OVERSAMPLE readings each, ~1 ms in total) and fills
 * `out`. Disabled channels are still measured but excluded from min/max. */
void gbl_vsens_read(gbl_vsens_t *out);

/* True if channel `ch` has a PDB connected and takes part in the checks. */
bool gbl_vsens_enabled(int ch);

/* Logs all four channel voltages on one line, prefixed by `tag`. */
void gbl_vsens_log(const char *tag, const gbl_vsens_t *r);
