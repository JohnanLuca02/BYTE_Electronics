/*
 * GBL - Production application
 */
#pragma once

/* Runs the power switch state machine. Never returns.
 * Requires gbl_power, gbl_vsens and gbl_switch to be initialised. */
void gbl_run_start(void);
