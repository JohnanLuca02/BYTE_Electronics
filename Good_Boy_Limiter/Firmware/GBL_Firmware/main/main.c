/*
 * GBL (Good Boy Limiter) - Firmware entry point
 *
 * app_main() is called by ESP-IDF once FreeRTOS and the system are running
 * (the C main() belongs to the framework). It initialises the hardware drivers
 * and logging, then starts the application selected in menuconfig:
 *   GBL firmware -> Application -> Production | Test/debug
 */

#include "sdkconfig.h"
#include "gbl_log.h"
#include "gbl_power.h"
#include "gbl_vsens.h"
#include "gbl_switch.h"
#include "gbl_run.h"
#include "gbl_debug.h"

void app_main(void)
{
	// Logging first so the start-up is recorded too: it only creates a buffer
	// and a task (no I/O), so the power outputs still reach their safe state
	// within microseconds.
	gbl_log_init();
	gbl_power_init();
	gbl_vsens_init();
	gbl_switch_init();

#if CONFIG_GBL_APP_DEBUG
	gbl_debug_start();
#else
	gbl_run_start();
#endif
}
