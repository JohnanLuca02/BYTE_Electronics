/*
 * GBL (Good Boy Limiter) - Firmware entry point
 *
 * app_main() is called by ESP-IDF once FreeRTOS and the system are running
 * (the C main() belongs to the framework). It initialises the hardware drivers
 * and starts the application selected in menuconfig:
 *   GBL firmware -> Application -> Production | Test/debug
 */

#include "sdkconfig.h"
#include "gbl_power.h"
#include "gbl_vsens.h"
#include "gbl_switch.h"
#include "gbl_run.h"
#include "gbl_debug.h"

void app_main(void)
{
	// The power outputs go to their safe state first, before anything else runs.
	gbl_power_init();
	gbl_vsens_init();
	gbl_switch_init();

#if CONFIG_GBL_APP_DEBUG
	gbl_debug_start();
#else
	gbl_run_start();
#endif
}
