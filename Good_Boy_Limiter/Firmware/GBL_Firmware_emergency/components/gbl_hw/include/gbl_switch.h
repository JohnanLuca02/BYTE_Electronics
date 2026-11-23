/*
 * GBL - Main power switch input
 *
 * Maintained switch between J2 pin 9 (GPIO1) and J2 pin 10 (GND).
 * Closed = ON. A broken wire or unplugged connector reads as OFF.
 * This is NOT the emergency stop.
 */
#pragma once

#include <stdbool.h>

/* Configures the input with its internal pull-up. Call once at start-up. */
void gbl_switch_init(void);

/* Instantaneous level, not debounced. */
bool gbl_switch_raw_on(void);

/* Debounced level: a change is accepted only once stable for the debounce
 * time. Call periodically (any period shorter than the debounce time). */
bool gbl_switch_is_on(void);
