/*
 * GBL EMERGENCY firmware - entry point
 *
 * TEMPORARY firmware for the current board revision only. Same drivers and
 * same state machine as production, but the pre-charge accepts a stalled
 * charge and closes the relay anyway (see gbl_run_emergency.c).
 * Use the production firmware (../GBL_Firmware) as soon as the pre-charge
 * stage is redesigned.
 */

#include "gbl_log.h"
#include "gbl_power.h"
#include "gbl_vsens.h"
#include "gbl_switch.h"

void gbl_run_emergency_start(void);

void app_main(void)
{
	gbl_log_init();
	gbl_power_init();
	gbl_vsens_init();
	gbl_switch_init();
	gbl_run_emergency_start();
}
