/*
 * GBL - Test/debug application
 *
 * Manual control of every element from the console (USB Serial/JTAG).
 * The Rpre1 protections built into gbl_power (Q1 watchdog, cooldown, voltage
 * checks during pre-charge) always stay active.
 * The power switch is only read and reported, it does not control anything.
 */

#include "gbl_debug.h"

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "gbl_power.h"
#include "gbl_vsens.h"
#include "gbl_switch.h"

#define LOOP_PERIOD_MS 20

static const char *TAG = "debug";

static void print_help(void)
{
	printf("\n=== GBL - TEST/DEBUG firmware ===\n"
		   "  S  full pre-charge (Q1 + checks + relay)\n"
		   "  X  controlled shutdown (Q1 parallel path, relay open, Q1 off)\n"
		   "  s  immediate power-off (also '0')\n"
		   "  P  Q1 on      p  Q1 off\n"
		   "  R  relay on   r  relay off\n"
		   "  V  PDB voltages + switch state\n"
		   "  ?  this help\n\n");
}

static void handle_command(int c)
{
	switch (c)
	{
	case 'S':
		gbl_power_precharge();
		break;
	case 'X':
		gbl_power_shutdown();
		break;
	case 's':
	case '0':
		gbl_power_off();
		break;
	case 'P':
		gbl_power_q1_set(true);
		break;
	case 'p':
		gbl_power_q1_set(false);
		break;
	case 'R':
		gbl_power_relay_set(true);
		break;
	case 'r':
		gbl_power_relay_set(false);
		break;
	case 'V':
	case 'v':
	{
		gbl_vsens_t r;
		gbl_vsens_read(&r);
		gbl_vsens_log("read", &r);
		ESP_LOGI(TAG, "Switch %s", gbl_switch_raw_on() ? "ON" : "OFF");
		break;
	}
	case '?':
		print_help();
		break;
	default:
		break;
	}
}

void gbl_debug_start(void)
{
	print_help();
	bool sw_last = gbl_switch_raw_on();

	while (1)
	{
		// Non-blocking stdin: getchar() returns EOF when no character is available.
		handle_command(getchar());

		// Report raw switch transitions to check wiring and contact bounce.
		bool sw = gbl_switch_raw_on();
		if (sw != sw_last)
			ESP_LOGI(TAG, "Switch -> %s", sw ? "ON" : "OFF");
		sw_last = sw;

		vTaskDelay(pdMS_TO_TICKS(LOOP_PERIOD_MS));
	}
}
