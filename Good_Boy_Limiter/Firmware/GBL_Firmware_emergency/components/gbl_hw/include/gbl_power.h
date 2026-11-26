/*
 * GBL - Power path control
 *
 * Q1 (pre-charge path through the LM5069 and Rpre1), the line relay, and the
 * two sequences built on them: pre-charge and controlled shutdown.
 * Every function protects Rpre1 on its own (Q1 watchdog, cooldown, voltage
 * checks), so callers cannot overheat it whatever they do.
 */
#pragma once

#include <stdbool.h>

typedef enum
{
	GBL_PRECHARGE_OK,	   // PDBs charged, relay closed, Q1 off
	GBL_PRECHARGE_REFUSED, // Rpre1 cooling down: nothing was switched on
	GBL_PRECHARGE_FAULT,   // short, ambiguous charge or timeout: everything off
} gbl_precharge_result_t;

/* Drives the outputs to their safe state and creates the Q1 watchdog.
 * Requires gbl_vsens_init() for the sequences. Call once at start-up. */
void gbl_power_init(void);

/* Q1 on/off. Switching on honours the Rpre1 cooldown and returns false if it
 * was refused; switching off always succeeds. */
bool gbl_power_q1_set(bool on);

/* Line relay closed/open. No protection: closing it on discharged PDBs causes
 * a full inrush through the contacts. Use gbl_power_precharge() instead. */
void gbl_power_relay_set(bool closed);

/* Milliseconds left before the Rpre1 cooldown expires (0 = ready). */
int gbl_power_cooldown_left_ms(void);

/* Pre-charge: Q1 on, monitored charge, relay closed, Q1 off.
 * Blocking, ~3.1 s at most. */
gbl_precharge_result_t gbl_power_precharge(void);

/* Controlled shutdown: Q1 on as a parallel path, relay opened, Q1 off.
 * The bus then discharges through the load. Blocking, ~300 ms. */
void gbl_power_shutdown(void);

/* Immediate power-off: relay open and Q1 off, no sequence. */
void gbl_power_off(void);
